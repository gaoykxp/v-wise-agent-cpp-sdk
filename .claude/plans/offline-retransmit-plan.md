# 周期采集数据断连重传机制方案

## 目标
为周期采集数据（`TOPIC_DVICE_VW_DATA_UP`）增加断连本地缓存与网络恢复后顺序补传：
- 断网时数据写入本地文件缓存
- 最多缓存 30 分钟数据；超过 30 分钟**停止缓存（丢弃新数据）**——已确认策略
- 网络恢复后按时间顺序（旧→新）逐条回传到 server

## 范围
仅作用于 `vwiseProbeTaskHandle()` 中的周期数据上报。故障上报（已有 `saveFault`/重试到 ACK 机制）、探针任务日志（ping/wireless 文件 + MINIO）不在本次范围。

## 新增文件
- `include/offline_cache.h` — `OfflineCache` 类声明
- `src/offline_cache.cpp` — 实现（`aux_source_directory(src ...)` 自动纳入构建，**无需改 CMakeLists**）

## 核心设计

### 1. OfflineCache 类（作为 ProbeMgr 成员）
```cpp
struct CachedRecord {
    int64_t ts;            // 毫秒时间戳
    std::string topic;
    std::string payload;   // 原始 JSON 字符串
};

class OfflineCache {
public:
    OfflineCache();
    void init(const std::string& dir, const std::string& file, int64_t maxAgeMs); // 建目录+加载已有缓存
    void push(const std::string& topic, const std::string& payload, int64_t ts);  // 断网写入；超 maxAgeMs 丢新
    size_t flush(const std::function<bool(const std::string&, const std::string&)>& sender); // 逐条回传
    bool empty() const;
    size_t size() const;
private:
    void loadFromFile();
    void persistLocked();  // 全量重写文件（持锁内调用），写 .tmp 再 rename 保证原子
    std::deque<CachedRecord> m_queue;
    mutable std::mutex m_mtx;
    std::string m_filePath, m_tmpPath;
    int64_t m_maxAgeMs = 30 * 60 * 1000;
};
```
**存储格式**：JSONL，每行 `{"ts":...,"topic":"...","payload":"..."}`；payload 作为字符串字段由 nlohmann::json 转义，避免换行破坏行格式。

### 2. push —— 丢新策略
```cpp
void push(topic, payload, ts) {
    std::lock_guard lk(m_mtx);
    // 缓存非空且新数据与最早记录间隔超 30min -> 停止缓存
    if (!m_queue.empty() && ts - m_queue.front().ts > m_maxAgeMs) {
        LogInfo << "offline cache span > 30min, drop new data";
        return;
    }
    m_queue.push_back({ts, topic, payload});
    persistLocked();
}
```
> 全量重写在 ≤600 条（30min×3s）规模下每 3s 写 ~100KB，开销可忽略；如需优化可改追加写，但全量重写最易保证 deque 与文件一致。

### 3. flush —— 逐条回传（旧→新，FIFO）
```cpp
size_t flush(sender) {
    size_t sent = 0;
    while (true) {
        CachedRecord rec;
        { std::lock_guard lk(m_mtx);
          if (m_queue.empty()) break;
          rec = m_queue.front(); }            // 锁内取队首
        if (!sender(rec.topic, rec.payload))   // 锁外发送，避免长持锁阻塞 push
            break;                              // 失败/断连：保留剩余，下次恢复续传
        { std::lock_guard lk(m_mtx);
          if (!m_queue.empty() && m_queue.front().ts == rec.ts) {
              m_queue.pop_front();
              persistLocked(); } }              // 成功才出队+落盘
        ++sent;
    }
    return sent;
}
```
落盘采用 `写 m_tmpPath → rename m_filePath`，避免崩溃产生半截文件。

### 4. MQTTAPP 新增（不动现有回调）
```cpp
bool isConnected() const { return client != nullptr && MQTTAsync_isConnected(client); }
bool sendOnce(const std::string& topic, const std::string& payload); // 返回 sendMessage 是否成功
```
- `isConnected()` 复用 paho 自带连接状态查询，**无需改动 connLost/onConnectSuccess 等回调**。
- `messageSend` 改为内部调用 `sendOnce` 并忽略返回值，对外签名不变（不影响其他调用点）。

### 5. ProbeMgr 集成
- 新增成员：`OfflineCache m_offlineCache;`、`std::atomic<bool> m_flushing{false};`
- `probeMgrStart()` 初始化：
  ```cpp
  base_tools::util::creatFilePath("/mnt/data/log/ProbeCache");
  m_offlineCache.init("/mnt/data/log/ProbeCache",
                      "/mnt/data/log/ProbeCache/data_cache.jsonl", 30*60*1000);
  ```
- 新增 `publishData()`，替换 `vwiseProbeTaskHandle()` 末尾的直接发送：
  ```cpp
  void ProbeMgr::publishData(const std::string& payload) {
      int64_t ts = base_tools::BaseTimer::GetMilliTime();
      if (mqtt.isConnected() && mqtt.sendOnce(TOPIC_DVICE_VW_DATA_UP, payload)) {
          if (!m_offlineCache.empty()) flushOfflineCache();  // 在线且仍有积压 -> 后台补传
      } else {
          m_offlineCache.push(TOPIC_DVICE_VW_DATA_UP, payload, ts); // 断网缓存
      }
  }
  ```
- 新增 `flushOfflineCache()`（后台线程 + atomic 防重入）：
  ```cpp
  void ProbeMgr::flushOfflineCache() {
      bool expected = false;
      if (!m_flushing.compare_exchange_strong(expected, true)) return; // 已在补传
      std::thread([this]{
          size_t n = m_offlineCache.flush([](const std::string& t, const std::string& p){
              return ProbeMgr::getInstance().mqtt.isConnected()
                  && ProbeMgr::getInstance().mqtt.sendOnce(t, p);
          });
          LogInfo << "offline cache flushed: " << n << " records";
          ProbeMgr::getInstance().m_flushing = false;
      }).detach();
  }
  ```
- 改动点（`probe_mgr.cpp`）：
  ```cpp
  // 原: ProbeMgr::getInstance().mqtt.messageSend(TOPIC_DVICE_VW_DATA_UP, zdpb_str);
  ProbeMgr::getInstance().publishData(zdpb_str);
  ```

### 6. 可配置项（`data/config.json` → `Vwise` 节点下，可选）
```json
"OfflineCache": { "Enable": 1, "MaxAgeMin": 30 }
```
`Enable=0` 时 `publishData` 直接走原 `messageSend` 路径不缓存；默认启用、30 分钟。

## 数据流
- **在线**：`vwiseProbeTaskHandle → publishData → sendOnce → server`
- **断网**：`vwiseProbeTaskHandle → publishData → push 写文件`（满 30min 后丢新）
- **恢复**：下次 `publishData`（在线）→ `flushOfflineCache` 后台逐条 `sendOnce` → 成功出队落盘

## 边界与并发
- `flush` 在独立线程；`sendOnce`/`isConnected` 线程安全（paho async 支持），与采集线程并发发送无冲突。
- `flush` 与 `push` 通过 `OfflineCache` 内 `m_mtx` 互斥；发送在锁外。
- **进程重启**：`init` 时 `loadFromFile` 恢复未发送缓存，联网后继续补传。
- **QoS0 固有限制**：`sendOnce` 成功仅代表 paho 接受入队，不保证送达；“已发送未落盘”窗口崩溃会产生重复，server 需容忍幂等（监控快照通常可接受）。

## 改动清单
| 文件 | 改动 |
|---|---|
| `include/offline_cache.h` | 新增 |
| `src/offline_cache.cpp` | 新增 |
| `include/mqtt_async_app.h` | 加 `isConnected()`、`sendOnce()` |
| `src/mqtt_async_app.cpp` | 实现 `sendOnce()`，`messageSend` 复用之 |
| `include/probe_mgr.h` | 加 `m_offlineCache`、`m_flushing`、`publishData()`、`flushOfflineCache()` |
| `src/probe_mgr.cpp` | `probeMgrStart` 初始化缓存；`vwiseProbeTaskHandle` 末尾改 `publishData`；新增两方法 |
| `data/config.json` | `Vwise` 下加 `OfflineCache` 节点（可选） |
