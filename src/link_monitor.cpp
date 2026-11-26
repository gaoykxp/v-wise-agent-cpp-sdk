/**********************************************************************************************************************
    > File: link_monitor.cpp
    > 双链路质量监测实现（Linux/RPi）：
    *   - 每 link 一个 socket：SO_BINDTODEVICE(ifname) + bind(:local_port)
    *   - 常规探测：PROBE_REQ → 等 PROBE_ACK（同 seq 回显 t_send），RTT EWMA
    *   - 丢包：滑动窗口 sent/acked 计数
    *   - 带宽 train：按目标速率发 N 个 1200B PROBE_REQ，末包携带 F_TRAIN_END，
    *     云端测接收速率经 ACK 回传，终端 EWMA
**********************************************************************************************************************/
#include "link_monitor.h"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace cmsr {
namespace vwise {

static uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

LinkMonitor &LinkMonitor::getInstance() {
    static LinkMonitor inst;
    return inst;
}

bool LinkMonitor::openSocket(LinkState &st) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    if (!st.cfg.ifname.empty()) {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, st.cfg.ifname.c_str(),
                       (socklen_t)st.cfg.ifname.size()) != 0) {
            log(LOG_WRN, "link[" + st.cfg.name + "] SO_BINDTODEVICE(" + st.cfg.ifname +
                             ") failed: " + strerror(errno));
            ::close(fd);
            return false;
        }
    }
    sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_ANY);
    la.sin_port = htons(st.cfg.local_port);
    if (::bind(fd, (sockaddr *)&la, sizeof(la)) != 0) {
        log(LOG_WRN, "link[" + st.cfg.name + "] bind port " + std::to_string(st.cfg.local_port) +
                         " failed: " + strerror(errno));
        ::close(fd);
        return false;
    }
    /* 收 ACK 后快速退出等待：SO_RCVTIMEO 在 probeOnce 内按需设置 */
    st.fd = fd;
    return true;
}

void LinkMonitor::closeSocket(LinkState &st) {
    if (st.fd >= 0) {
        ::close(st.fd);
        st.fd = -1;
    }
}

bool LinkMonitor::start(const std::vector<LinkConfig> &links, const ProbeConfig &pc) {
    if (running_.load()) return true;
    if (links.empty()) return false;
    pc_ = pc;

    links_.clear();
    for (auto &lc : links) {
        if (lc.anchor_ip.empty() || lc.anchor_port == 0) {
            log(LOG_WRN, "link[" + lc.name + "] invalid anchor config, skipped");
            continue;
        }
        links_.emplace_back();
        LinkState &st = links_.back();
        st.cfg = lc;
        if (!openSocket(st)) {
            log(LOG_WRN, "link[" + lc.name + "] socket open failed, marked dead");
        }
    }
    if (links_.empty()) return false;

    running_.store(true);
    for (auto &st : links_) {
        st.th = std::thread([this, &st]() { probeLoop(st); });
    }
    log(LOG_INF, "LinkMonitor started: " + std::to_string(links_.size()) + " links");
    return true;
}

void LinkMonitor::stop() {
    if (!running_.exchange(false)) return;
    /* socket 关闭使阻塞 recv 立即退出 */
    for (auto &st : links_) {
        closeSocket(st);
    }
    for (auto &st : links_) {
        if (st.th.joinable()) st.th.join();
    }
    links_.clear();
    log(LOG_INF, "LinkMonitor stopped");
}

std::vector<LinkQuality> LinkMonitor::snapshot() const {
    std::vector<LinkQuality> out;
    for (auto &st : links_) {
        LinkQuality q;
        q.id = st.cfg.id;
        q.name = st.cfg.name;
        q.alive = st.alive.load();
        q.rtt_ewma_ms = st.rtt_ewma_ms.load();
        q.bw_ewma_kbps = st.bw_ewma_kbps.load();
        q.probes_sent = st.probes_sent.load();
        q.probes_acked = st.probes_acked.load();
        /* 丢包率：最近 kLossWin 个探测的滑动窗口（累计值对早期丢包永不遗忘，
         * 链路恢复后长期虚高，DUAL_DUP 退出判定迟钝） */
        int fill = st.win_fill.load(), miss = st.win_misses.load();
        q.loss_pct = fill > 0 ? 100.0 * (double)miss / (double)fill : 0.0;
        out.push_back(q);
    }
    return out;
}

bool LinkMonitor::allDown() const {
    bool any = false;
    for (auto &st : links_) {
        if (st.alive.load()) any = true;
    }
    return !any;
}

bool LinkMonitor::sendToLink(int link_idx, const void *buf, size_t len) {
    if (link_idx < 0) return false;
    auto it = links_.begin();
    std::advance(it, link_idx);
    if (it == links_.end()) return false;
    LinkState &st = *it;
    if (st.fd < 0) return false;
    sockaddr_in da;
    memset(&da, 0, sizeof(da));
    da.sin_family = AF_INET;
    da.sin_port = htons(st.cfg.anchor_port);
    da.sin_addr.s_addr = inet_addr(st.cfg.anchor_ip.c_str());
    return ::sendto(st.fd, buf, len, 0, (sockaddr *)&da, sizeof(da)) >= 0;
}

double LinkMonitor::probeOnce(LinkState &st, uint32_t seq, uint64_t *ack_rate_kbps) {
    uint8_t pkt[VWD_PROBE_MIN_BYTES];
    memset(pkt, 0, sizeof(pkt));
    VwPktHdr *h = (VwPktHdr *)pkt;
    vwd_init_hdr(h, VWD_T_PROBE_REQ, (uint8_t)st.cfg.id, 0);
    h->flow_id = 0;
    h->seq = seq;
    h->t_send_ns = now_ns();

    sockaddr_in da;
    memset(&da, 0, sizeof(da));
    da.sin_family = AF_INET;
    da.sin_port = htons(st.cfg.anchor_port);
    da.sin_addr.s_addr = inet_addr(st.cfg.anchor_ip.c_str());

    uint64_t t0 = now_ns();
    if (::sendto(st.fd, pkt, sizeof(pkt), 0, (sockaddr *)&da, sizeof(da)) < 0) return -1;
    st.probes_sent.fetch_add(1);

    /* 等待同 seq ACK（可能收到旧 ACK / CTRL 指令，分别处理） */
    int timeout = pc_.probe_timeout_ms > 0 ? pc_.probe_timeout_ms : 200;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    while (running_.load() && std::chrono::steady_clock::now() < deadline) {
        timeval tv{0, 50 * 1000}; /* 50ms 粒度轮询，兼顾精度与退出响应 */
        setsockopt(st.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        uint8_t rbuf[VWD_PROBE_MIN_BYTES + VWD_CTRL_MAX + 8];
        sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = ::recvfrom(st.fd, rbuf, sizeof(rbuf), 0, (sockaddr *)&from, &flen);
        if (n <= 0) continue;
        const VwPktHdr *ack = vwd_parse(rbuf, (size_t)n);
        if (!ack) continue;
        /* v2：云端 CTRL 指令（模式切换/train）在此通道下发 */
        if (ack->type == VWD_T_CTRL && ctrl_handler_) {
            int plen = n - VWD_HDR_SIZE;
            if (plen > 0 && plen <= VWD_CTRL_MAX) {
                std::string json((const char *)rbuf + VWD_HDR_SIZE, (size_t)plen);
                ctrl_handler_(json);
            }
            continue;
        }
        if (ack->type != VWD_T_PROBE_ACK || ack->seq != seq) continue;
        double rtt_us = (double)(now_ns() - t0) / 1000.0;
        st.probes_acked.fetch_add(1);
        if (ack_rate_kbps && n >= VWD_PROBE_SIZE) {
            const VwProbeExtra *ex = (const VwProbeExtra *)(rbuf + VWD_HDR_SIZE);
            *ack_rate_kbps = ex->rx_rate_kbps;
        }
        return rtt_us;
    }
    return -1;
}

double LinkMonitor::runTrain(LinkState &st, uint32_t &train_id) {
    train_id = next_train_id_++;
    if (train_id == 0) train_id = ++next_train_id_;

    const int kPktSize = VWD_PROBE_TRAIN_BYTES;
    std::vector<uint8_t> pkt(kPktSize, 0);
    VwPktHdr *h = (VwPktHdr *)pkt.data();
    vwd_init_hdr(h, VWD_T_PROBE_REQ, (uint8_t)st.cfg.id, 0);
    h->flow_id = train_id;
    h->seq = 0;
    /* 净荷区必须携带 train_id——云端据此归组并测速（否则被当普通探测，rx_rate 恒 0） */
    VwProbeExtra *ex = (VwProbeExtra *)(pkt.data() + VWD_HDR_SIZE);
    memset(ex, 0, sizeof(*ex));
    ex->train_id = train_id;

    sockaddr_in da;
    memset(&da, 0, sizeof(da));
    da.sin_family = AF_INET;
    da.sin_port = htons(st.cfg.anchor_port);
    da.sin_addr.s_addr = inet_addr(st.cfg.anchor_ip.c_str());

    /* 目标速率 pacing：gap_ns = size*8/rate（duration_cast 转 time_point 的整型 duration） */
    double rate_bps = pc_.train_rate_kbps * 1000.0;
    double gap_ns = (double)kPktSize * 8.0 / rate_bps * 1e9;
    auto gap = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::nano>(gap_ns));
    auto next_t = std::chrono::steady_clock::now();

    uint64_t ack_rate = 0;
    for (int i = 0; i < pc_.train_pkts; ++i) {
        if (!running_.load()) return -1; /* stop() 响应：训练中途中止 */
        next_t += gap;
        std::this_thread::sleep_until(next_t);
        h->seq = (uint32_t)i;
        h->flags = (i == pc_.train_pkts - 1) ? VWD_F_TRAIN_END : 0;
        h->t_send_ns = now_ns();
        if (::sendto(st.fd, pkt.data(), kPktSize, 0, (sockaddr *)&da, sizeof(da)) < 0) {
            return -1;
        }
        /* train 的 ACK 只有末包有意义；顺带回收中间 ACK 防缓冲堆积 */
        if (i == pc_.train_pkts - 1) {
            /* 末包：等 ACK 携带 rx_rate */
            uint64_t seq = (uint32_t)i;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
            while (running_.load() && std::chrono::steady_clock::now() < deadline) {
                timeval tv{0, 100 * 1000};
                setsockopt(st.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                uint8_t rbuf[2048];
                sockaddr_in from;
                socklen_t flen = sizeof(from);
                int n = ::recvfrom(st.fd, rbuf, sizeof(rbuf), 0, (sockaddr *)&from, &flen);
                if (n <= 0) continue;
                const VwPktHdr *ack = vwd_parse(rbuf, (size_t)n);
                if (!ack || ack->type != VWD_T_PROBE_ACK || ack->seq != seq) continue;
                if (n >= VWD_PROBE_SIZE) {
                    const VwProbeExtra *ex = (const VwProbeExtra *)(rbuf + VWD_HDR_SIZE);
                    ack_rate = ex->rx_rate_kbps;
                }
                break;
            }
        } else {
            /* 非末包：非阻塞清空可能早到的 ACK（防接收缓冲堆积） */
            uint8_t rbuf[2048];
            (void)::recvfrom(st.fd, rbuf, sizeof(rbuf), MSG_DONTWAIT, nullptr, nullptr);
        }
    }
    return (double)ack_rate;
}

void LinkMonitor::probeLoop(LinkState &st) {
    if (st.fd < 0) {
        /* socket 打开失败：周期重试打开 */
        while (running_.load()) {
            if (openSocket(st)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }
    }
    uint64_t last_train = 0;

    while (running_.load()) {
        if (st.fd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            if (!openSocket(st)) continue;
        }

        uint32_t seq = ++st.probe_seq;
        double rtt_us = probeOnce(st, seq);

        /* 丢包滑动窗口更新（探测线程独占写 loss_win/loss_pos） */
        {
            int miss = (rtt_us >= 0) ? 0 : 1;
            if (st.win_fill.load() < LinkState::kLossWin) {
                st.win_fill.fetch_add(1);
            } else {
                st.win_misses.fetch_sub(st.loss_win[st.loss_pos]);
            }
            st.loss_win[st.loss_pos] = (uint8_t)miss;
            st.win_misses.fetch_add(miss);
            st.loss_pos = (st.loss_pos + 1) % LinkState::kLossWin;
        }

        if (rtt_us >= 0) {
            double rtt_ms = rtt_us / 1000.0;
            double old = st.rtt_ewma_ms.load();
            double v = (old < 0) ? rtt_ms : old * 0.7 + rtt_ms * 0.3;
            st.rtt_ewma_ms.store(v);
            st.miss_streak.store(0);
            st.alive.store(true);
        } else {
            int misses = st.miss_streak.fetch_add(1) + 1;
            if (misses >= pc_.alive_miss_n) st.alive.store(false);
        }

        /* 带宽 train（周期性 + 手动 requestTrain） */
        if (pc_.train_period_sec > 0 || train_req_.load()) {
            uint64_t now = now_ns();
            bool want = (last_train == 0 || now - last_train > (uint64_t)pc_.train_period_sec * 1000000000ull);
            if (train_req_.exchange(false)) want = true;
            if (want) {
                last_train = now;
                uint32_t tid = 0;
                double bw = runTrain(st, tid);
                if (bw > 0) {
                    double old = st.bw_ewma_kbps.load();
                    double v = (old < 0) ? bw : old * 0.6 + bw * 0.4;
                    st.bw_ewma_kbps.store(v);
                    log(LOG_DBG, "link[" + st.cfg.name + "] train#" + std::to_string(tid) +
                                     " rx_rate=" + std::to_string((uint32_t)bw) + "kbps");
                }
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(pc_.interval_ms > 0 ? pc_.interval_ms : 1000));
    }
}

} // namespace vwise
} // namespace cmsr
