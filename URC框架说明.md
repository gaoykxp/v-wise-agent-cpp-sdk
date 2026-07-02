# 蜂窝模组 URC 处理框架说明

> 适用范围：`v-wise-agent-cpp-sdk/boards/rpi` 下的 4G/5G 蜂窝模组 AT 通道
> 相关文件：[rpi_urc.h](boards/rpi/rpi_urc.h) · [rpi_urc.cpp](boards/rpi/rpi_urc.cpp) · [rpi_at_client.h](boards/rpi/rpi_at_client.h) · [rpi_at_client.cpp](boards/rpi/rpi_at_client.cpp) · [rpi_module_interface.cpp](boards/rpi/rpi_module_interface.cpp)

---

## 1. 概述

URC（Unsolicited Result Code，主动上报码）是蜂窝模组在没有被询问的情况下、主动通过 AT 串口上报的状态信息，例如：

| URC | 含义 |
|-----|------|
| `RDY` | 模组启动就绪 |
| `+CFUN: 1` | 无线功能模式变化 |
| `+QSIMSTAT: 1,1` | SIM 卡热插拔状态变化 |
| `+CMTI: "SM",12` | 收到新短信 |
| `+CEREG: 1,"0123","ABCD1234"` | LTE 注册状态变化 |

本框架为 `AtClient` 的 AT 通道提供 URC 的**捕获、剥离、异步分发**能力，并解决旧实现中 URC 被"隐式吞掉"或"误解析为命令响应"的问题。

---

## 2. 背景与问题

改造前，[rpi_at_client.cpp](boards/rpi/rpi_at_client.cpp) 的 `command()` 是**同步轮询模式**：

```
write(AT+xxx) → 循环 read_some → 读到 OK/ERROR 或超时 → 返回
```

存在三类问题：

1. **URC 被丢弃**：URC 落在两次 `command()` 之间时，无人读串口，直接丢失。
2. **URC 被误解析**：URC 恰好落在某次 `command()` 读窗口内时，会和命令响应混在一起塞进 `rsp.lines`，若前缀碰巧匹配会被当成本次命令的数据，导致解析错误。
3. **无主动上报**：没有任何地方发送 `AT+CEREG=1`、`AT+CNMI=...` 等使能上报的命令，模组即使能上报也大多被关闭。

本框架用「单一读线程 + 命令等待」替代同步读，从根本上解决上述问题。

---

## 3. 架构总览

框架分三层，URC 相关逻辑全部收敛在 [rpi_urc.h](boards/rpi/rpi_urc.h) / [rpi_urc.cpp](boards/rpi/rpi_urc.cpp)，`AtClient` 只做"接线"。

```
┌─────────────────────────────────────────────────────────────┐
│  AtClient                                                    │
│                                                              │
│   command() ──┐                       ┌── register_urc_     │
│   (调用方线程) │                       │    handler()        │
│                │                       │                      │
│                ▼                       │                      │
│        ┌───────────────┐  cmd_mtx_    │   ┌───────────────┐  │
│        │  PendingCmd   │◀──────────────┘   │ UrcDispatcher │  │
│        │  (条件变量等待)│                    │ (前缀→回调表)  │  │
│        └──────▲────────┘                    │      │        │  │
│               │ done/ok                      │      ▼        │  │
│               │                              │  worker 线程   │  │
│        ┌──────┴──────────────────────────────┼──异步队列─────┤  │
│        │     reader_loop()  (唯一读串口者)    │              │  │
│        │                                       │              │  │
│        │  port_.read_some() ── LineAccumulator │              │  │
│        │                                       │              │  │
│        │  行分类:                              │              │  │
│        │   ① 最终结果 → 唤醒 command() ─────────┘              │  │
│        │   ② 已注册 URC → dispatch 给 worker ──┐               │  │
│        │   ③ 命令回显 → 丢弃                   │               │  │
│        │   ④ 数据行   → 塞进 PendingCmd.lines  │               │  │
│        ▼                                       ▼               │  │
│     SerialPort                            用户回调             │  │
└─────────────────────────────────────────────────────────────┘
```

| 层 | 职责 | 文件 |
|----|------|------|
| 分类层 | `is_final_result()` 判终止码；`LineAccumulator` 按行切流 | rpi_urc |
| 分发层 | `UrcDispatcher`：前缀→回调表 + 异步 worker 队列 | rpi_urc |
| 接线层 | `AtClient` 持有的后台读线程，独占 `read_some` | rpi_at_client |

---

## 4. 核心机制

### 4.1 单一读线程

`AtClient::reader_loop()` 是串口**唯一**的读取者。`command()` 不再自己 `read_some`，而是登记一个 `PendingCmd` 后在条件变量上等待。读线程把每条完整行按以下优先级分类：

1. **最终结果码**（`OK`/`ERROR`/`+CME ERROR`/`+CMS ERROR`）→ 置 `done=true` 并唤醒等待中的 `command()`。
2. **已注册 URC**（前缀命中）→ 从响应流中**剥离**，投递给 `UrcDispatcher` 异步处理。
3. **命令回显**（等于刚发的命令文本）→ 丢弃。
4. **普通数据行** → 塞进当前 `PendingCmd.lines`，供 `command()` 返回。

这样 URC 既不会丢失（读线程一直在读），也不会污染命令响应（已被剥离）。

### 4.2 命令串行化

同一时刻**最多一条 AT 命令在飞**。`command()` 用 `cmd_in_flight_` + `cmd_serial_cv_` 实现：

- 第二个 `command()` 调用（如 URC 回调里发的命令）会阻塞，直到前一条完成或超时，**绝不会覆盖**前者的 `PendingCmd`。
- 这保证 URC 回调里可以安全地发 AT 命令，它们会排队执行。

```
command(A) ──▶ 占用槽 ──▶ 等响应 ──▶ 释放槽
                                    │
command(B, 来自回调) ─阻塞等槽─▶ 占用槽 ──▶ ...
```

### 4.3 URC 异步分发

`UrcDispatcher` 拥有一条独立 worker 线程。读线程调用 `dispatch()` 是**非阻塞**的（仅入队 + notify）。worker 取出行后在**自己的线程**上调用用户回调。

为什么必须解耦：若回调在读线程上同步执行，而回调里又发了 `AT+xxx`，读线程就会卡在回调里读不到响应 → 死锁。worker 线程与读线程分离后，回调里发命令能正常排队（走串行化槽），读线程始终空闲可读。

### 4.4 关闭流程

`disconnect()` 按以下顺序，确保不挂起、不崩溃：

1. 置 `shutting_down_=true`，唤醒所有阻塞的 `command()` 和串行化等待者（让它们立即返回失败）。
2. `reading_=false`，join 读线程（读线程因 `read_some` 的 200ms 超时及时退出）。
3. `urc_.stop()`：worker 处理完队列中残余 URC 后退出。
4. 复位 `pending_`、`last_cmd_`，`port_.close()`。

---

## 5. 公开 API

### 注册 URC 处理器

```cpp
// rpi_at_client.h
void register_urc_handler(const std::string& prefix, UrcCallback cb);
void unregister_urc_handler(const std::string& prefix);
```

`prefix` 可以是带冒号的前缀（如 `"+CMT:"`）或裸词（如 `"RDY"`），大小写敏感。重复注册同一前缀会替换旧回调。

```cpp
using UrcCallback = std::function<void(const std::string& line)>;
```

### 使用示例

```cpp
tbox::AtClient atc;
atc.connect("/dev/ttyUSB2", 115200);   // 内部已发 ATE0 关回显、已启动读线程与 URC worker

// 注册：收到短信到达 URC 时打印
atc.register_urc_handler("+CMTI:", [](const std::string& line) {
    // line 形如: +CMTI: "SM",12
    // 在 worker 线程执行，可安全发 AT 命令读取短信内容
    std::cout << "SMS arrived: " << line << std::endl;
    // atc.command("AT+CMGR=12", std::chrono::milliseconds(2000));  // 安全
});

// 普通查询照常用，行为完全不变
auto imei = atc.get_imei();
auto sig  = atc.get_lte_signal();
```

**关键约束（见 §7）**：不要注册 `+CEREG:`/`+CREG:` 这类与轮询命令响应前缀冲突的 URC。

---

## 6. 默认已使能的 URC

[rpi_module_interface.cpp](boards/rpi/rpi_module_interface.cpp) 的 `RPIModuleInterface::init()` 在连接成功后默认注册并使能以下 URC：

| 前缀 | 使能命令 | 用途 |
|------|---------|------|
| `RDY` | （模组固有） | 模组就绪 |
| `+CFUN:` | （模组固有） | 无线功能模式变化 |
| `+QSIMSTAT:` | `AT+QSIMSTAT=1` | SIM 卡热插拔 |
| `+CMTI:` | `AT+CNMI=2,1,0,0,0` | 短信到达通知 |

使能命令失败会被忽略（非所有模组都支持每条命令），不影响其他 URC。

---

## 7. URC 前缀规避原则（重要）

**不要注册与轮询命令响应前缀相同的 URC**。

原因：`AT+CEREG?` 的响应行是 `+CEREG: ...`，而 URC 上报也是 `+CEREG: ...`，两者**无法靠前缀区分**。若同时注册了 `+CEREG:` URC，读线程会把命令响应行也当 URC 剥离掉，导致 `get_lte_registration()` 拿不到数据。

| 命令 | 响应前缀 | 是否可注册为 URC |
|------|---------|----------------|
| `AT+CEREG?` | `+CEREG:` | ❌ 维持轮询 |
| `AT+CREG?` | `+CREG:` | ❌ 维持轮询 |
| `AT+CSQ` | `+CSQ:` | ❌ 维持轮询 |
| `AT+CPIN?` | `+CPIN:` | ❌ 维持轮询 |
| （模组主动）`+QSIMSTAT:` | — | ✅ |
| （模组主动）`+CMTI:` | — | ✅ |
| （模组主动）`RDY` / `+CFUN:` | — | ✅ |

**原则**：只注册那些**只会由模组主动上报、不会作为查询命令响应出现**的前缀。若将来确需订阅 CREG 类 URC，应给 `command()` 增加 `expected_prefix` 参数——命令在飞期间该前缀的行归响应、其余注册前缀归 URC。这是可平滑扩展的方向，v1 未实现。

---

## 8. 线程模型与线程安全

系统共有三条线程：

| 线程 | 职责 | 可被谁阻塞 |
|------|------|------------|
| 调用方线程 | 调 `command()` / `get_xxx()` | `command()` 的 `wait_for`（最长 timeout） |
| reader 线程 | `reader_loop()`，独占 `read_some` | `read_some` 的 200ms 超时 |
| URC worker 线程 | 执行用户 URC 回调 | 回调自身耗时；发命令时排队等串行槽 |

锁与不变量：

- `cmd_mtx_` 保护 `pending_`、`last_cmd_`、`cmd_in_flight_`、`shutting_down_`。
- `UrcDispatcher` 内部 `mtx_` 保护 handler 表和队列。
- 两把锁**从不嵌套**（reader 调 `urc_.is_urc`/`dispatch` 时只持 urc 内部锁，不持 `cmd_mtx_`），无死锁。
- 写串口（`port_.write`）在调用方线程、读串口在 reader 线程，termios 上 read/write 分离，安全。
- `last_cmd_` 在 reader 侧比对回显时，先在锁内拷贝副本再比较，避免 `std::string` 数据竞争。

---

## 9. 已知限制

### 超时后迟到响应的串扰（仅异常路径）

当某条 `command()` **超时**返回时，模组可能稍后才发出该命令的迟到响应行和最终结果。由于串行化槽在超时返回后已释放，下一条命令可能在迟到响应到达前建立新的 `PendingCmd`，导致迟到响应被并入下一条命令的 `rsp.lines`。

- **影响范围**：仅命令超时这一异常路径（模组无响应/线缆故障）。正常通信（命令按时返回）不受影响。
- **历史对照**：旧同步实现存在相同的残余缓冲问题。
- **v1 取舍**：接受此限制。修复需引入"超时后排空栅栏"，因涉及与 reader 的读流协调，复杂度较高，留待后续。

### 回调线程契约

- URC 回调在 worker 线程执行，**可安全发 AT 命令**（会走串行化槽排队）。
- 但**不要在回调里调用 `disconnect()` 或 `register_urc_handler`**（前者会 join 自身导致 `std::terminate`）。回调应只做"记录状态 / 上报事件 / 读取数据"，重活交给上层。

---

## 10. 编译与接入

新文件已加入 [CMakeLists.txt](CMakeLists.txt) 的 `STUB_SRCS`：

```cmake
set(STUB_SRCS
    ...
    ${PROJECT_SOURCE_DIR}/boards/rpi/rpi_at_client.cpp
    ${PROJECT_SOURCE_DIR}/boards/rpi/rpi_urc.cpp          # 新增
    ...
)
```

`boards/rpi` 已在 `include_directories`，直接 `#include "rpi_urc.h"` 即可。在树莓派上：

```bash
cd v-wise-agent-cpp-sdk
mkdir build && cd build
cmake ..
make
```

---

## 11. 扩展指南：新增一个 URC

以"监听模组温度告警 `+QTEMP:`"为例：

1. **确认前缀安全**：该前缀不会作为某条轮询命令的响应出现（查 §7 表）。`+QTEMP:` 只由模组主动上报 → 安全。

2. **注册 handler**（在 `init()` 或运行时均可）：

   ```cpp
   m_atClient.register_urc_handler("+QTEMP:", [](const std::string& line) {
       // line 形如: +QTEMP: 58
       // 自行解析，建议轻量
   });
   ```

3. **使能上报**（查阅模组 AT 手册，确认使能命令）：

   ```cpp
   m_atClient.command("AT+QTEMP=1", std::chrono::milliseconds(1500));
   ```

4. **取消订阅**：

   ```cpp
   m_atClient.unregister_urc_handler("+QTEMP:");
   ```

> 注意：裸词前缀（如 `RDY`）按前缀匹配，`RDYXYZ` 也会命中 `RDY`。若裸词可能误匹配，建议带完整前缀（带冒号）注册。

---

## 12. 文件清单

| 文件 | 角色 |
|------|------|
| [boards/rpi/rpi_urc.h](boards/rpi/rpi_urc.h) | URC 框架接口：`LineAccumulator`、`is_final_result`、`UrcDispatcher` |
| [boards/rpi/rpi_urc.cpp](boards/rpi/rpi_urc.cpp) | URC 框架实现：worker 线程、分发、stop 后丢弃保护 |
| [boards/rpi/rpi_at_client.h](boards/rpi/rpi_at_client.h) | `AtClient`：新增读线程成员、`reader_loop`、`register_urc_handler` |
| [boards/rpi/rpi_at_client.cpp](boards/rpi/rpi_at_client.cpp) | `AtClient`：重写 `command`/`connect`/`disconnect`，新增 `reader_loop` |
| [boards/rpi/rpi_module_interface.cpp](boards/rpi/rpi_module_interface.cpp) | `init()`：注册默认 URC、使能上报 |
| [CMakeLists.txt](CMakeLists.txt) | 加入 `rpi_urc.cpp` 源文件 |
