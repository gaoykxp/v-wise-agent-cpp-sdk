/**********************************************************************************************************************
    > File: dup_sel_controller.cpp
    > 模式状态机（设计稿 3.3）实现：
    *   - 双链路均优        → SINGLE_BEST（省带宽）
    *   - 任一链路劣化(超门限) → DUAL_DUP（双发保可靠）
    *   - 单链路失联        → FAILOVER（强制幸存链路）
    *   - 指令手动模式优先于自动决策（manual_mode_）
    * JSON 依赖说明：为保持模块零依赖（duptest 可独立链接），此处用 nlohmann 仅在
    * agent 编译时可用——故 onCommand/buildStatusJson 用手写极简 JSON 组装与解析。
**********************************************************************************************************************/
#include "dup_sel_controller.h"
#include "dedup_engine.h"
#include "ebpf_loader.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace cmsr {
namespace vwise {

DupSelController &DupSelController::getInstance() {
    static DupSelController inst;
    return inst;
}

const char *DupSelController::modeName() const {
    switch (mode_.load()) {
        case VWD_MODE_SINGLE_BEST: return "single_best";
        case VWD_MODE_DUAL_DUP: return "dual_dup";
        case VWD_MODE_FAILOVER: return "failover";
    }
    return "unknown";
}

void DupSelController::start(EbpfLoader *ebpf, const DupSelThresholds &th) {
    if (running_.exchange(true)) return;
    ebpf_ = ebpf;
    th_ = th;
    ebpf_ok_.store(ebpf_ != nullptr && ebpf_->loaded());
    if (ebpf_ != nullptr && !ebpf_ok_.load()) {
        fprintf(stderr, "[dupsel] eBPF not loaded -> user-space fallback mode\n");
    }
    mode_.store(VWD_MODE_SINGLE_BEST);
    manual_mode_.store(-1);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        confirm_mode_ = -1;
        confirm_count_ = 0;
    }
    th_ctrl_ = std::thread(&DupSelController::controlLoop, this);
    /* v2：TERM_STATS 2s 周期上报（经链路0 探测 socket → 云端看板） */
    th_report_ = std::thread(&DupSelController::reportLoop, this);
    fprintf(stderr, "[dupsel] started (ebpf=%s thresholds rtt>%.0fms loss>%.1f%% confirm=%d)\n",
            ebpf_ok_.load() ? "yes" : "no", th_.rtt_bad_ms, th_.loss_bad_pct, th_.confirm_n);
}

void DupSelController::stop() {
    if (!running_.exchange(false)) return;
    if (th_ctrl_.joinable()) th_ctrl_.join();
    if (th_report_.joinable()) th_report_.join();
}

/* 极简 JSON 字段提取（双引号字符串值），无第三方依赖 */
static bool json_get_str(const std::string &j, const char *key, std::string &out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return false;
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    p = j.find('"', p);
    if (p == std::string::npos) return false;
    size_t e = j.find('"', p + 1);
    if (e == std::string::npos) return false;
    out = j.substr(p + 1, e - p - 1);
    return true;
}

int DupSelController::onCommand(const std::string &json) {
    /* v2：看板/平台经 CTRL 通道下发的动作指令（train） */
    {
        std::string action;
        if (json_get_str(json, "action", action) && action == "train") {
            LinkMonitor::getInstance().requestTrain();
            fprintf(stderr, "[dupsel] command: train triggered\n");
            return 0;
        }
    }

    std::string mode;
    if (!json_get_str(json, "mode", mode)) return -1;

    int want = -1;
    int forced_link = -1;
    if (mode == "auto") {
        manual_mode_.store(-1);
        fprintf(stderr, "[dupsel] command: auto (manual override cleared)\n");
        return 0;
    } else if (mode == "dual_dup") {
        want = VWD_MODE_DUAL_DUP;
    } else if (mode == "single_best") {
        want = VWD_MODE_SINGLE_BEST;
    } else if (mode == "failover") {
        /* failover 命令可携带 link 字段；缺省取链路0 */
        want = VWD_MODE_FAILOVER;
        forced_link = 0;
    } else {
        return -2;
    }

    manual_mode_.store(want);
    applyMode(want, forced_link, "command");
    return 0;
}

int DupSelController::decideTarget(const std::vector<LinkQuality> &qs, int &forced_link) {
    forced_link = -1;
    if (qs.empty()) return VWD_MODE_SINGLE_BEST;

    /* 1) FAILOVER：恰一条链路 alive */
    int alive_idx = -1, alive_cnt = 0;
    for (size_t i = 0; i < qs.size(); ++i) {
        if (qs[i].alive) { alive_idx = (int)i; ++alive_cnt; }
    }
    if (alive_cnt == 0) return VWD_MODE_SINGLE_BEST; /* 全断：维持，探测自会重试 */
    if (alive_cnt == 1) {
        forced_link = alive_idx;
        return VWD_MODE_FAILOVER;
    }

    /* 2) 质量判定：任一链路差 → DUAL_DUP；均优 → SINGLE_BEST */
    bool any_bad = false;
    for (auto &q : qs) {
        if (!q.alive) { any_bad = true; break; }
        if (q.rtt_ewma_ms >= 0 && q.rtt_ewma_ms > th_.rtt_bad_ms) any_bad = true;
        if (q.loss_pct >= 0 && q.loss_pct > th_.loss_bad_pct) any_bad = true;
    }
    return any_bad ? VWD_MODE_DUAL_DUP : VWD_MODE_SINGLE_BEST;
}

void DupSelController::applyMode(int mode, int forced_link, const char *reason) {
    int prev = mode_.exchange(mode);
    if (prev == mode) return;

    if (ebpf_ != nullptr && ebpf_ok_.load()) {
        ebpf_->set_dup_enabled(mode == VWD_MODE_DUAL_DUP);
    }
    fprintf(stderr, "[dupsel] mode %s -> %s (%s%s%s)\n",
            prev == VWD_MODE_SINGLE_BEST ? "single_best"
            : prev == VWD_MODE_DUAL_DUP ? "dual_dup" : "failover",
            mode == VWD_MODE_SINGLE_BEST ? "single_best"
            : mode == VWD_MODE_DUAL_DUP ? "dual_dup" : "failover",
            reason,
            forced_link >= 0 ? " link=" : "",
            forced_link >= 0 ? std::to_string(forced_link).c_str() : "");
}

void DupSelController::controlLoop() {
    auto next_t = std::chrono::steady_clock::now();
    while (running_.load()) {
        next_t += std::chrono::milliseconds(th_.report_period_ms > 0 ? th_.report_period_ms : 5000);
        std::this_thread::sleep_until(next_t);

        if (!LinkMonitor::getInstance().isRunning()) continue;

        int forced_link = -1;
        int manual = manual_mode_.load();
        int target;
        if (manual >= 0) {
            target = manual;
        } else {
            target = decideTarget(LinkMonitor::getInstance().snapshot(), forced_link);
            /* 滞回确认：连续 confirm_n 次同一目标才切换 */
            std::lock_guard<std::mutex> lk(mtx_);
            if (target != confirm_mode_) {
                confirm_mode_ = target;
                confirm_count_ = 1;
            } else if (++confirm_count_ >= th_.confirm_n) {
                applyMode(target, forced_link, "probe_threshold");
            }
            /* 手动模式直接生效（onCommand 已 apply） */
        }

        /* 状态上报 */
        if (statusPublisher_) statusPublisher_(buildStatusJson(""));
    }
}

bool DupSelController::buildTermStats(uint8_t *buf, size_t cap, size_t &out_len) const {
    if (buf == nullptr || cap < VWD_TERM_STATS_SIZE) return false;
    memset(buf, 0, VWD_TERM_STATS_SIZE);
    VwPktHdr *h = (VwPktHdr *)buf;
    vwd_init_hdr(h, VWD_T_TERM_STATS, 0, 0);
    h->t_send_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    VwTermStatsExtra *ex = (VwTermStatsExtra *)(buf + VWD_HDR_SIZE);
    ex->mode = (uint32_t)mode_.load();
    ex->ebpf_on = ebpf_ok_.load() ? 1 : 0;

    auto qs = LinkMonitor::getInstance().snapshot();
    if (qs.empty()) return false;
    size_t n = 0;
    for (auto &q : qs) {
        if (n >= 2) break;
        VwTermStatsLink &l = ex->links[n];
        l.id = (uint32_t)q.id;
        l.alive = q.alive ? 1 : 0;
        l.rtt_us = q.rtt_ewma_ms < 0 ? VWD_TERM_NO_SAMPLE : (uint32_t)(q.rtt_ewma_ms * 1000.0);
        l.loss_pct_x100 = q.loss_pct < 0 ? VWD_TERM_NO_SAMPLE : (uint32_t)(q.loss_pct * 100.0);
        l.bw_kbps = q.bw_ewma_kbps < 0 ? VWD_TERM_NO_SAMPLE : (uint32_t)q.bw_ewma_kbps;
        l.probes_sent = (uint32_t)q.probes_sent;
        l.probes_acked = (uint32_t)q.probes_acked;
        ++n;
    }
    ex->nlinks = (uint32_t)n;

    if (dedup_ != nullptr) {
        auto s = dedup_->stats();
        ex->dedup_rx = (uint32_t)s.rx_total;
        ex->dedup_unique = (uint32_t)s.rx_unique;
        ex->dedup_dup = (uint32_t)s.rx_dup;
        ex->dedup_oos = (uint32_t)s.oos_events;
        ex->dedup_last_seq = (uint32_t)s.last_seq;
    }
    out_len = VWD_TERM_STATS_SIZE;
    return true;
}

void DupSelController::reportLoop() {
    while (running_.load()) {
        uint8_t pkt[VWD_TERM_STATS_SIZE];
        size_t len = 0;
        if (buildTermStats(pkt, sizeof(pkt), len)) {
            if (!LinkMonitor::getInstance().sendToLink(0, pkt, len)) {
                /* 链路0 不可用则试链路1 */
                LinkMonitor::getInstance().sendToLink(1, pkt, len);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
}

std::string DupSelController::buildStatusJson(const std::string &imei) const {
    auto qs = LinkMonitor::getInstance().snapshot();
    std::string j = "{";
    if (!imei.empty()) j += "\"imei\":\"" + imei + "\",";
    char ts[32];
    snprintf(ts, sizeof(ts), "%llu", (unsigned long long)time(nullptr));
    j += "\"ts\":" + std::string(ts) + ",";
    j += "\"mode\":\"" + std::string(modeName()) + "\",";
    j += "\"ebpf\":\"" + std::string(ebpf_ok_.load() ? "on" : "fallback") + "\",";
    j += "\"links\":[";
    for (size_t i = 0; i < qs.size(); ++i) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"name\":\"%s\",\"alive\":%s,\"rtt_ms\":%.1f,\"loss_pct\":%.2f,"
                 "\"bw_kbps\":%.0f,\"sent\":%llu,\"acked\":%llu}%s",
                 qs[i].id, qs[i].name.c_str(), qs[i].alive ? "true" : "false",
                 qs[i].rtt_ewma_ms < 0 ? -1.0 : qs[i].rtt_ewma_ms,
                 qs[i].loss_pct < 0 ? -1.0 : qs[i].loss_pct,
                 qs[i].bw_ewma_kbps < 0 ? -1.0 : qs[i].bw_ewma_kbps,
                 (unsigned long long)qs[i].probes_sent,
                 (unsigned long long)qs[i].probes_acked,
                 i + 1 < qs.size() ? "," : "");
        j += buf;
    }
    j += "]}";
    return j;
}

} // namespace vwise
} // namespace cmsr
