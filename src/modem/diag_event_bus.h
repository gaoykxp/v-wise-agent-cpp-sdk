/**********************************************************************************************************************
    > File Name: diag_event_bus.h
    > Description: 诊断事件总线（模组无关）
    >
    > 诊断证据最值钱的是"事件流"而非"状态快照"：网络踢了模组（NW DETACH）还是模组
    > 自己掉线（ME DETACH）、掉电重启（RDY）还是网络闪断——都在 URC 事件流里。
    > 方言层驱动把私有 URC 翻译成 DiagEvent 发布到本总线；fault 上报时取最近
    > N 条事件附在 payload["diag"]["events"]，平台侧即可还原故障时间线。
**********************************************************************************************************************/
#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "modem_hal.h"

namespace vwise::modem {

struct DiagEventRecord {
    int64_t ts_ms{0};        // 毫秒时间戳（与上报 payload 同源）
    DiagEvent type{DiagEvent::ModuleBoot};
    std::string domain;      // 责任域（module/sim/network/host）
    std::string raw;         // 原始 URC 行（证据保留）
};

class DiagEventBus {
public:
    static DiagEventBus& getInstance();

    // 发布一条诊断事件（线程安全；驱动 URC 回调线程调用）
    void publish(DiagEvent ev, std::string raw = "");

    // 取最近 n 条事件（时间升序）
    std::vector<DiagEventRecord> recent(size_t n) const;

    // 当前缓存事件数
    size_t size() const;

private:
    DiagEventBus() = default;
    static constexpr size_t kRingCapacity = 200;   // 环形缓冲上限

    mutable std::mutex mtx_;
    std::deque<DiagEventRecord> ring_;
};

} // namespace vwise::modem
