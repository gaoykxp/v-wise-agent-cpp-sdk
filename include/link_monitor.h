/**********************************************************************************************************************
    > File: link_monitor.h
    > 双链路质量监测器（P0）：每链路一个探测 socket（SO_BINDTODEVICE 绑定出接口），
    > 周期探测 RTT/丢包（EWMA），周期带宽 train（云端测 rx_rate 回传）。
    > 独立于 SDK 其余模块（无 paho/spdlog/nlohmann 依赖），可被 v_wise_agent 与
    * vwise_duptest 共同链接。
    > Author: P0-P2 双链路远控实施
**********************************************************************************************************************/
#ifndef TANGO_NEXUS_LINK_MONITOR_H
#define TANGO_NEXUS_LINK_MONITOR_H

#include "vwise_dup_protocol.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cmsr {
namespace vwise {

/* 单链路配置 */
struct LinkConfig {
    int id = 0;
    std::string name;       /* 逻辑名（如 wwan0） */
    std::string ifname;     /* 绑定物理出接口（空=不绑定，走系统路由） */
    std::string anchor_ip;  /* 云端锚点地址 */
    uint16_t anchor_port = 0;
    uint16_t local_port = 0; /* 探测 socket 本地端口（0=系统分配；用于云端学习终端端点） */
};

/* 单链路运行时质量快照（消费者拷贝使用） */
struct LinkQuality {
    int id = 0;
    std::string name;
    bool alive = false;        /* 连续 N 探测无 ACK 判定失联 */
    double rtt_ewma_ms = -1;   /* -1=无样本 */
    double loss_pct = -1;      /* 最近窗口丢包率 */
    double bw_ewma_kbps = -1;  /* 云端实测 train 接收速率 EWMA */
    uint64_t probes_sent = 0;
    uint64_t probes_acked = 0;
};

/* 全局探测配置 */
struct ProbeConfig {
    int interval_ms = 1000;        /* 常规探测周期 */
    int train_pkts = 60;           /* train 包数 */
    double train_rate_kbps = 4000; /* train 发送目标速率 */
    int train_period_sec = 10;     /* train 周期（0=不做带宽估计） */
    int probe_timeout_ms = 200;    /* 单探测等待 ACK 超时 */
    int alive_miss_n = 3;          /* 连续无 ACK 判失联 */
};

class LinkMonitor {
public:
    static LinkMonitor &getInstance();

    /* links 为空或配置非法 → 返回 false（调用方降级为无链路监测） */
    bool start(const std::vector<LinkConfig> &links, const ProbeConfig &pc);
    void stop();
    bool isRunning() const { return running_.load(); }

    /* 获取全部链路质量快照（线程安全拷贝） */
    std::vector<LinkQuality> snapshot() const;
    /* 是否所有链路均无响应 */
    bool allDown() const;

    /* ---- v2：Web 看板支撑 ---- */
    /* 经指定链路探测 socket 发送原始报文（TERM_STATS 上报用） */
    bool sendToLink(int link_idx, const void *buf, size_t len);
    /* 云端 CTRL 指令处理回调（JSON 文本）→ DupSelController::onCommand */
    using CtrlHandler = std::function<int(const std::string &json)>;
    void setCtrlHandler(CtrlHandler fn) { ctrl_handler_ = std::move(fn); }
    /* 手动触发一次带宽 train（下个探测周期生效） */
    void requestTrain() { train_req_.store(true); }

    /* 日志回调（agent 注入 spdlog；duptest 缺省 printf） */
    void setLogger(std::function<void(int lvl, const std::string &msg)> fn) { logger_ = std::move(fn); }

    static constexpr int LOG_DBG = 0, LOG_INF = 1, LOG_WRN = 2, LOG_ERR = 3;

private:
    LinkMonitor() = default;
    ~LinkMonitor() { stop(); }
    LinkMonitor(const LinkMonitor &) = delete;
    LinkMonitor &operator=(const LinkMonitor &) = delete;

    struct LinkState {
        static constexpr int kLossWin = 20; /* 丢包滑动窗口（最近 N 个探测） */
        LinkConfig cfg;
        int fd = -1;                       /* 探测 socket */
        std::atomic<uint64_t> probes_sent{0};
        std::atomic<uint64_t> probes_acked{0};
        std::atomic<double> rtt_ewma_ms{-1.0};
        std::atomic<int> miss_streak{0};
        std::atomic<bool> alive{false};
        std::atomic<double> bw_ewma_kbps{-1.0};
        /* 丢包滑动窗口：loss_win/loss_pos 仅探测线程读写；
         * win_misses/win_fill 原子供 snapshot 跨线程读 */
        uint8_t loss_win[kLossWin] = {0};
        int loss_pos = 0;
        std::atomic<int> win_misses{0};
        std::atomic<int> win_fill{0};
        uint32_t probe_seq = 0;
        std::thread th;
    };

    void probeLoop(LinkState &st);
    bool openSocket(LinkState &st);
    void closeSocket(LinkState &st);
    /* 单次往返探测，返回 RTT(us)，-1=失败 */
    double probeOnce(LinkState &st, uint32_t seq, uint64_t *ack_rate_kbps = nullptr);
    /* 带宽 train：返回云侧实测 rx_rate kbps，-1=失败 */
    double runTrain(LinkState &st, uint32_t &train_id);

    void log(int lvl, const std::string &msg) {
        if (logger_) logger_(lvl, msg);
        else fprintf(stderr, "[linkmon] %s\n", msg.c_str());
    }

    /* LinkState 含 atomic/thread 不可移动：用 list 保证节点地址稳定
     * （探测线程持有元素引用，vector 扩容/移动会失效） */
    std::list<LinkState> links_;
    ProbeConfig pc_;
    std::atomic<bool> running_{false};
    std::atomic<bool> train_req_{false};
    std::function<void(int, const std::string &)> logger_;
    CtrlHandler ctrl_handler_;
    uint32_t next_train_id_ = 1;
};

} // namespace vwise
} // namespace cmsr

#endif // TANGO_NEXUS_LINK_MONITOR_H
