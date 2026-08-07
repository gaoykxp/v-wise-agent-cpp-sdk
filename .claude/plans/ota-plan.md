# OTA 固件升级方案（Agent 自更新）

## 决策（已确认）
- 升级对象：Agent 自身可执行文件（`v_wise_agent`）
- 下载方式：平台 MQTT 下发元信息（版本/URL/SHA-256），SDK 用 curl 下载
- 校验强度：SHA-256 校验和（完整性，防下载损坏）
- 重启方式：systemd 托管（`systemctl restart` 或进程退出由 systemd 拉起）

## 总体流程

```
平台 ──MQTT(/sys/commands)──> {cmd_type:"ota_upgrade", version, url, sha256, size}
  SDK 收到 -> OtaManager.handleOtaCommand() [工作线程]
    1. ACK 受理
    2. 下载 url -> /mnt/data/ota/update.bin (curl 流式写文件，周期上报进度)
    3. SHA-256(update.bin) == sha256 ? 否 -> 上报 verify_failed，清理，结束
    4. 安装：cp 当前exe -> exe.bak；rename(update.bin 经目标fs临时文件 -> exe)；chmod 0755；写 pending 标志
    5. 上报 installing -> system("systemctl restart v_wise_agent")
  systemd 重启新二进制
  启动时 OtaManager.checkPendingOta()：健康计时器到期则提交(commit)；连续崩溃超限则回滚(.bak 复位)
```

## 新增/改动文件

| 文件 | 改动 |
|---|---|
| `include/ota_manager.h` | **新增** `OtaManager` 单例 |
| `src/ota_manager.cpp` | **新增** 下载/校验/安装/回滚/状态上报（aux_source_directory 自动纳入构建，curl+openssl 已链接，无需改 CMake） |
| `basetools/httpclient.h/.cpp` | 增加 `HTCLDownloadFile(url, filePath, headers, caPath)`：curl 流式写文件（现有 `HTCLWriteData` 只写内存 string，不适合大文件） |
| `include/probe_mgr.h` | include `ota_manager.h` |
| `src/probe_mgr.cpp` | `proccessTaskMgr` 增加 `cmd_type=="ota_upgrade"` 分支；启动检查移至 main 早期 |
| `src/main.cpp` | 启动尽早 `OtaManager::init()` + `checkPendingOta()`（提交/回滚不依赖 MQTT） |
| `data/config.json` | `Vwise` 下加 `Ota` 配置节点 |

## 1. OtaManager 设计

```cpp
class OtaManager {
public:
    static OtaManager& getInstance();
    void init();                                   // 读配置、工作目录、exe 路径
    void handleOtaCommand(const nlohmann::json& j);// 收到 OTA 指令（内部起工作线程）
    void checkPendingOta();                        // 启动时调用：提交或回滚
private:
    void otaWorker(std::string taskId, std::string version,
                   std::string url, std::string sha256, int64_t size);
    bool downloadFile(const std::string& url, const std::string& path);
    std::string sha256File(const std::string& path);          // 流式 EVP_Digest
    bool installBinary();                                     // cp 备份 + 原子 rename
    void reportStatus(const std::string& taskId, const std::string& status,
                      const std::string& version, const std::string& extra = "");
    void restartSelf();                                       // systemctl restart / exit
    void commitOta();                                         // 计时器到期：提交
    void rollbackOta();                                       // 回滚到 exe.bak

    std::atomic<bool> m_running{false};   // 防并发 OTA
    std::string m_workDir = "/mnt/data/ota";
    std::string m_unit = "v_wise_agent";
    std::string m_exePath;                // 当前可执行文件路径(programPath)
    int m_bootCommitSec = 60;
    int m_maxBootAttempts = 3;
    Timer m_commitTimer;
};
```

- 状态上报复用 `ProbeMgr::getInstance().mqtt.messageSend(TOPIC_PLAT_ORDER_DOWN_ACK, payload)`，payload 含 `cmd_type:"ota_upgrade"`、`task_id`、`status`、`version`。status：`accepted/downloading/verify_failed/installing/success/rollback/failed`。
- 工作线程：`handleOtaCommand` 用 `m_running` 原子防重入，detach 线程执行 `otaWorker`（不阻塞 MQTT 回调与采集定时器）。

## 2. 关键实现点

### 下载（流式写文件）
`CHttpClient::HTCLDownloadFile`：新增文件写回调 `WriteToFile`（fwrite 到 FILE*）。
- `CURLOPT_CONNECTTIMEOUT=30`，`CURLOPT_TIMEOUT=0`（不限总时长，大文件下载）。
- `CURLOPT_FOLLOWLOCATION=1`（允许重定向）。失败重试由调用方控制；下载失败清理残文件。

### SHA-256 校验（流式，EVP）
```cpp
EVP_MD_CTX* ctx = EVP_MD_CTX_new(); EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
// 循环 fread 64KB -> EVP_DigestUpdate -> EVP_DigestFinal -> hex
```
与指令 `sha256`（hex）大小写不敏感比较；不一致即 `verify_failed`，删除 `update.bin`，结束。

### 安装（跨文件系统安全 + 原子替换）
当前 exe 路径取自 `common::GlobalData::Instance()->programPath()`（`readlink(/proc/self/exe)`）。
```
1. 拷贝 update.bin -> exe.new.tmp     // 拷到目标文件系统，为原子 rename 做准备
2. cp exe -> exe.bak                   // 备份原文件（exe 仍在位，无空窗）
3. rename(exe.new.tmp -> exe)          // 同文件系统原子覆盖，运行中旧进程 inode 仍存活
4. chmod(exe, 0755)
5. 写 pending 标志 {task_id, version, attempts:0, ts}
```
- exe 全程存在，无“无可用 exe”窗口；跨文件系统由步骤1的拷贝解决。

### 重启
```cpp
std::system(("systemctl restart " + m_unit).c_str());
std::exit(0);   // 兜底：依赖 systemd Restart=always 拉起
```
`m_unit` 由配置 `Ota.SystemdUnit` 提供（默认 `v_wise_agent`）。

### 启动提交与自动回滚（防砖）
`main` 启动尽早 `checkPendingOta()`（不依赖 MQTT）：
- 无 pending 标志：正常启动。
- 有 pending：`attempts++` 写回。若 `attempts >= m_maxBootAttempts`(3) -> **回滚** `rename(exe.bak -> exe)`、删 pending、`restartSelf()`。
- 否则启动 `m_bootCommitSec`(60s) 计时器；到期且进程仍存活 -> **提交**：删 pending、删 exe.bak、`reportStatus(success)`。
- 进程计时器到期前崩溃 -> systemd 重启 -> 下次 `attempts++`，直至触发回滚。

## 3. 命令接入

`proccessTaskMgr`（与 `collection_config` 同级分支）：
```cpp
if (j["cmd_type"].get<std::string>() == "ota_upgrade") {
    OtaManager::getInstance().handleOtaCommand(j);
    return;
}
```
OTA 指令示例：
```json
{"cmd_type":"ota_upgrade","task_id":"ota_001","version":"1.0.1",
 "url":"https://ota.platform/v_wise_agent-1.0.1","sha256":"<hex>","size":1234567}
```

## 4. 配置（config.json -> Vwise.Ota）
```json
"Ota": {
    "WorkDir": "/mnt/data/ota",
    "SystemdUnit": "v_wise_agent",
    "BootCommitSec": 60,
    "MaxBootAttempts": 3
}
```

## 5. 目录与产物
`/mnt/data/ota/`：`update.bin`（下载临时）、`pending`（升级未决标志 JSON）。
`<exe>.bak`（上一版备份，与 exe 同目录）、`<exe>.new.tmp`（安装临时）。

## 6. 并发与边界
- OTA 全程在工作线程，不阻塞采集/MQTT；`m_running` 防并发 OTA。
- 下载前 `statvfs` 校验磁盘可用空间 >= 2×size。
- SHA-256 仅校验完整性（按决策不做 RSA 签名）；如后续需防篡改，可叠加 `Authenticator::verifySignature`。
- 安装需 root 且 exe 目录可写；只读 rootfs 部署需把 exe 放可写路径。
- 回滚发生在启动早期（MQTT 未就绪），`rollback` 状态不上报，平台可由版本未变化推断。

## 改动清单
- 新增 `include/ota_manager.h`、`src/ota_manager.cpp`
- 改 `basetools/httpclient.h/.cpp`：加 `HTCLDownloadFile`
- 改 `src/probe_mgr.cpp`：接入 cmd 分发
- 改 `src/main.cpp`：启动检查
- 改 `data/config.json`：加 `Vwise.Ota`
