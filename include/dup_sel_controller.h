/**********************************************************************************************************************
    > File: dup_sel_controller.h
    > 双发选收控制器（P1）：链路质量 → 模式状态机（设计稿 3.3）：
    *   SINGLE_BEST ⇄ DUAL_DUP ⇄ FAILOVER，滞回 + 连续 N 次确认防乒乓。
    *   生效通道（按可用性自动选择）：
    *     1. eBPF ctrl map（内核态双发，业务流打 VWD_MARK_RC）
    *     2. 用户态降级（EbpfLoader 未加载时，duptest/代理双发路径参考 effectiveMode）
    *   控制面（MQTT，由 probe_manager 接线）：
    *     - 周期发布链路状态 JSON（回调 statusPublisher_）
    *     - 接收模式指令 onCommand(json)
**********************************************************************************************************************/
#ifndef TANGO_NEXUS_DUP_SEL_CONTROLLER_H
#define TANGO_NEXUS_DUP_SEL_CONTROLLER_H

#include "link_monitor.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace cmsr {
namespace vwise {

class EbpfLoader; /* 前向声明（由 probe_manager/duptest 注入，避免硬依赖） */
class DedupEngine; /* duptest 挂接的下行去重计数（TERM_STATS 上报用） */

struct DupSelThresholds {
    double rtt_bad_ms = 150;    /* RTT 超过此值判"差" */
    double loss_bad_pct = 5.0; /* 丢包率超过此值判"差" */
    int confirm_n = 3;          /* 连续 N 次判定才切换（防抖） */
    int report_period_ms = 5000; /* 状态上报周期 */
};

class DupSelController {
public:
    static DupSelController &getInstance();

    /* ebpf 可为 nullptr（未启用内核双发时全程用户态降级） */
    void start(EbpfLoader *ebpf, const DupSelThresholds &th);
    void stop();
    bool isRunning() const { return running_.load(); }

    /* 当前生效模式（VWD_MODE_*） */
    int effectiveMode() const { return mode_.load(); }
    /* FAILOVER 生效时的强制链路索引（-1=未指定，按 0 处理）——
     * 用户态发送方据此选择幸存链路，避免固定发链路0 */
    int forcedLink() const { return forced_link_.load(); }
    const char *modeName() const;

    /* eBPF 是否加载成功（false=用户态降级） */
    bool ebpfLoaded() const { return ebpf_ok_.load(); }

    /* MQTT 指令处理：{"imei":..,"mode":"dual_dup|single_best|failover|auto"}
     * 返回 0=接受（调用方回 ACK），非 0=拒绝 */
    int onCommand(const std::string &json);

    /* probe_manager 注入的发布回调（topic 外部组装，保持模块零依赖） */
    using StatusPublisher = std::function<void(const std::string &payloadJson)>;
    void setStatusPublisher(StatusPublisher fn) { statusPublisher_ = std::move(fn); }

    /* v2：看板支撑——挂接 DedupEngine（TERM_STATS 携带下行去重计数；nullptr=报零） */
    void attachDedup(DedupEngine *d) { dedup_ = d; }

    /* 状态 JSON（含双链路质量与当前模式） */
    std::string buildStatusJson(const std::string &imei) const;

    /* 状态 JSON → 结构化填充（TERM_STATS 组包复用；返回 false=无链路数据） */
    bool buildTermStats(uint8_t *buf, size_t cap, size_t &out_len) const;

private:
    DupSelController() = default;
    ~DupSelController() { stop(); }
    DupSelController(const DupSelController &) = delete;
    DupSelController &operator=(const DupSelController &) = delete;

    void controlLoop();
    void reportLoop(); /* v2：2s 周期 TERM_STATS 上报（链路0） */
    /* 依据质量计算目标模式（含滞回确认），返回 VWD_MODE_*；forced_link 输出 FAILOVER 目标链路 */
    int decideTarget(const std::vector<LinkQuality> &qs, int &forced_link);
    void applyMode(int mode, int forced_link, const char *reason);

    EbpfLoader *ebpf_ = nullptr;
    DedupEngine *dedup_ = nullptr;   /* 可空：duptest 下行去重计数（agent 侧暂空） */
    DupSelThresholds th_;
    std::atomic<bool> running_{false};
    std::atomic<int> mode_{VWD_MODE_SINGLE_BEST};
    std::atomic<int> forced_link_{-1}; /* FAILOVER 强制链路（applyMode 写） */
    std::atomic<bool> ebpf_ok_{false};
    std::atomic<int> manual_mode_{-1}; /* -1=auto；手动指令优先 */
    std::thread th_ctrl_;
    std::thread th_report_;           /* v2：TERM_STATS 上报线程 */
    StatusPublisher statusPublisher_;

    /* 滞回确认状态 */
    std::mutex mtx_;
    int confirm_mode_ = -1;
    int confirm_count_ = 0;
};

} // namespace vwise
} // namespace cmsr

#endif // TANGO_NEXUS_DUP_SEL_CONTROLLER_H
