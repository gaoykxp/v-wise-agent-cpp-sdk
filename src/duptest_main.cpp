/**********************************************************************************************************************
    > File: duptest_main.cpp
    > vwise_duptest —— 双链路远控数据面联调工具（P0-P2）：
    *   --mode probe   仅运行 LinkMonitor（P0：链路质量/带宽估计验证）
    *   --mode up      上行双发发生器（P1：kernel=eBPF 克隆 / user=双 socket 双发 / none=对照）
    *   --mode down    下行接收 + DedupEngine 去重（P2）
    *   --mode full    probe + up + down 并行（默认）
    * 配置来自 config.json 的 Vwise.DupSel 节（--config 指定路径，默认 conf/config.json）
    * 依赖：link_monitor / dedup_engine / dup_sel_controller / ebpf_loader（零外部依赖）
    * 权限：SO_BINDTODEVICE / SO_MARK / eBPF 均需 root（sudo）
    * Author: P0-P2 双链路远控实施
**********************************************************************************************************************/
#include "dedup_engine.h"
#include "dup_sel_controller.h"
#include "ebpf_loader.h"
#include "link_monitor.h"
#include "vwise_dup_protocol.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

using namespace cmsr::vwise;

/* ---------------- 极简 JSON 读取（仅取 Vwise.DupSel 配置所需的扁平值） ---------------- */

/* 在 json 文本中查找 "key": 后的数值/字符串（不含嵌套层级判断，取首个匹配——
 * 联调 config 中键名唯一，足够使用；agent 集成路径走 nlohmann 正规解析） */
static bool json_find(const std::string &j, const std::string &key, size_t &pos) {
    std::string pat = "\"" + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return false;
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    pos = p + 1;
    return true;
}
static double json_num(const std::string &j, const std::string &key, double def) {
    size_t p;
    if (!json_find(j, key, p)) return def;
    return atof(j.c_str() + p);
}
static std::string json_str(const std::string &j, const std::string &key, const std::string &def) {
    size_t p;
    if (!json_find(j, key, p)) return def;
    size_t b = j.find('"', p);
    if (b == std::string::npos) return def;
    size_t e = j.find('"', b + 1);
    if (e == std::string::npos) return def;
    return j.substr(b + 1, e - b - 1);
}
/* 提取 "Links": [ {...}, {...} ] 中每个 {...} 元素文本 */
static std::vector<std::string> json_link_elems(const std::string &j) {
    std::vector<std::string> out;
    size_t p = j.find("\"Links\"");
    if (p == std::string::npos) return out;
    size_t b = j.find('[', p);
    if (b == std::string::npos) return out;
    int d = 0;
    size_t s = 0;
    for (size_t i = b; i < j.size(); ++i) {
        if (j[i] == '{') {
            if (d == 0) s = i;
            ++d;
        } else if (j[i] == '}') {
            --d;
            if (d == 0) out.push_back(j.substr(s, i - s + 1));
        } else if (j[i] == ']' && d == 0) break;
    }
    return out;
}

/* ---------------- 配置 ---------------- */
struct TestCfg {
    std::vector<LinkConfig> links;
    ProbeConfig probe;
    DupSelThresholds th;
    int ebpf_enable = 1;
    std::string ebpf_obj = "vwise_dup.bpf.o";
    std::vector<std::string> ebpf_nics;
};

static TestCfg load_cfg(const std::string &path) {
    TestCfg tc;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return tc;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);

    /* 截取 Vwise.DupSel 段（容错：直接全文匹配键名亦可） */
    tc.probe.interval_ms = (int)json_num(data, "IntervalMs", 1000);
    tc.probe.train_pkts = (int)json_num(data, "TrainPkts", 60);
    tc.probe.train_rate_kbps = json_num(data, "TrainRateKbps", 4000);
    tc.probe.train_period_sec = (int)json_num(data, "TrainPeriodSec", 10);
    tc.th.rtt_bad_ms = json_num(data, "RttBadMs", 150);
    tc.th.loss_bad_pct = json_num(data, "LossBadPct", 5);
    tc.th.confirm_n = (int)json_num(data, "ConfirmN", 3);
    tc.ebpf_enable = (int)json_num(data, "EbpfEnable", 1);
    tc.ebpf_obj = json_str(data, "EbpfObj", "vwise_dup.bpf.o");

    /* EbpfNics: 字符串数组 "eth0","wlan0" */
    {
        std::string s = json_str(data, "EbpfNics", "");
        if (!s.empty()) {
            std::string cur;
            s += ',';
            for (char c : s) {
                if (c == ',') {
                    if (!cur.empty()) tc.ebpf_nics.push_back(cur);
                    cur.clear();
                } else if (c != '"' && c != ' ' && c != '[' && c != ']') {
                    cur += c;
                }
            }
        }
    }

    /* Links 数组 */
    int id = 0;
    for (auto &elem : json_link_elems(data)) {
        LinkConfig lc;
        lc.id = id;
        lc.name = json_str(elem, "Name", "link" + std::to_string(id));
        lc.ifname = json_str(elem, "Ifname", "");
        lc.anchor_ip = json_str(elem, "AnchorIp", "");
        lc.anchor_port = (uint16_t)json_num(elem, "AnchorPort", 0);
        lc.local_port = (uint16_t)json_num(elem, "LocalPort", 0);
        if (!lc.anchor_ip.empty() && lc.anchor_port != 0) {
            tc.links.push_back(lc);
            ++id;
        }
    }
    return tc;
}

/* ---------------- 上/下行会话 ----------------
 * 端点学习规则（与云端 anchor_server 一致）：云端把下行发往"最近一次收到该锚点
 * DATA 包的源地址"。因此：
 *   - kernel/none 模式：单一会话 socket（不绑设备，走默认路由；SO_MARK 由 eBPF 克隆）
 *     上行发送与下行接收共用，云端两锚点学到同一端点 → 双发副本都到本 socket；
 *   - user 模式：每链路一个 socket（SO_BINDTODEVICE），上行双发下行双收，
 *     云端锚点 i 学到链路 i 的端点 → 双发副本分别到达对应链路 socket。
 */
static std::atomic<bool> g_run{true};
static std::atomic<uint64_t> up_sent{0};

enum class UpDup { NONE, USER, KERNEL };

static int open_udp_sock(const std::string &ifname, uint16_t local_port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    if (!ifname.empty()) {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname.c_str(),
                       (socklen_t)ifname.size()) != 0) {
            fprintf(stderr, "[up] SO_BINDTODEVICE(%s) failed: %s\n", ifname.c_str(),
                    strerror(errno));
            close(fd);
            return -1;
        }
    }
    sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_ANY);
    la.sin_port = htons(local_port);
    if (::bind(fd, (sockaddr *)&la, sizeof(la)) != 0) {
        fprintf(stderr, "[up] bind %u failed: %s\n", local_port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

struct SessionCfg {
    const TestCfg *tc;
    UpDup dup_mode;
    double rate_pps;
    int pkt_bytes;
    int duration_s;
    uint32_t flow_id;
    DedupEngine *dedup; /* null=纯上行（收到的内容丢弃） */
};

static void session_updown(const SessionCfg &sc) {
    const TestCfg &tc = *sc.tc;
    std::vector<int> fds;
    std::vector<std::string> bind_ifs;

    if (sc.dup_mode == UpDup::USER) {
        for (size_t i = 0; i < tc.links.size(); ++i) {
            /* 业务会话 socket 用 LocalPort+200（探测 socket 占用 LocalPort 本尊）；
             * 云端从 DATA_UP 源地址学习数据端点 → 下行自动寻址到本 socket */
            int fd = open_udp_sock(tc.links[i].ifname, (uint16_t)(tc.links[i].local_port + 200));
            if (fd < 0) { fprintf(stderr, "[up] link socket open failed\n"); goto cleanup; }
            fds.push_back(fd);
            bind_ifs.push_back(tc.links[i].ifname);
        }
    } else {
        /* kernel：单 socket，不绑设备（克隆副本会从对端网卡回包，绑了设备会滤掉） */
        int fd = open_udp_sock("", (uint16_t)(tc.links[0].local_port + 200));
        if (fd < 0) { fprintf(stderr, "[up] socket open failed\n"); goto cleanup; }
        if (sc.dup_mode == UpDup::KERNEL) {
            int mark = VWD_MARK_RC;
            if (setsockopt(fd, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) != 0) {
                fprintf(stderr, "[up] SO_MARK failed: %s (need root)\n", strerror(errno));
                close(fd);
                goto cleanup;
            }
        }
        fds.push_back(fd);
        bind_ifs.push_back("");
    }

    {
        std::vector<sockaddr_in> dst(tc.links.size());
        for (size_t i = 0; i < tc.links.size(); ++i) {
            memset(&dst[i], 0, sizeof(dst[i]));
            dst[i].sin_family = AF_INET;
            dst[i].sin_port = htons(tc.links[i].anchor_port);
            dst[i].sin_addr.s_addr = inet_addr(tc.links[i].anchor_ip.c_str());
        }

        std::vector<uint8_t> pkt(sc.pkt_bytes > VWD_HDR_SIZE ? sc.pkt_bytes : VWD_HDR_SIZE + 64, 0);
        /* duration<double> → 整型 duration 需显式 duration_cast（同 downlink_sender） */
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / sc.rate_pps));
        auto next_t = std::chrono::steady_clock::now();
        uint32_t seq = 0;
        auto end_at = std::chrono::steady_clock::now() + std::chrono::seconds(sc.duration_s);

        fprintf(stderr, "[up] start: dup=%s rate=%.0fpps size=%zudB dur=%ds flow=0x%X\n",
                sc.dup_mode == UpDup::KERNEL ? "kernel" : sc.dup_mode == UpDup::USER ? "user" : "none",
                sc.rate_pps, pkt.size(), sc.duration_s, sc.flow_id);

        /* 接收线程：每个 fd 一条（下行去重统计 + 云端 CTRL 指令） */
        std::vector<std::thread> rx;
        if (sc.dedup != nullptr) {
            for (size_t i = 0; i < fds.size(); ++i) {
                rx.emplace_back([dedup = sc.dedup, fd = fds[i], lid = (int)i]() {
                    uint8_t buf[2048];
                    while (g_run.load()) {
                        sockaddr_in from;
                        socklen_t flen = sizeof(from);
                        int n = ::recvfrom(fd, buf, sizeof(buf), 0, (sockaddr *)&from, &flen);
                        if (n <= 0) {
                            if (errno == EINTR) continue;
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            continue;
                        }
                        const VwPktHdr *h = vwd_parse(buf, (size_t)n);
                        if (h && h->type == VWD_T_DATA_DOWN) {
                            DedupEngine::Stats delta;
                            dedup->feed(h->flow_id, h->seq, lid, delta);
                        }
                        /* 其他类型（如自身 eBPF 未克隆场景下的杂散包）忽略 */
                    }
                });
            }
        }

        /* 发送循环 */
        while (g_run.load() && std::chrono::steady_clock::now() < end_at) {
            std::this_thread::sleep_until(next_t);
            next_t += period;

            VwPktHdr *h = (VwPktHdr *)pkt.data();
            vwd_init_hdr(h, VWD_T_DATA_UP, 0, 0);
            h->flow_id = sc.flow_id;
            h->seq = ++seq;
            h->t_send_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();

            if (sc.dup_mode == UpDup::USER && tc.links.size() >= 2) {
                /* 用户态双发：跟随 DupSelController 生效模式（CTRL/自动状态机可切换）——
                 * dual_dup：两链路各一份（第二份置 F_DUPLICATE）；
                 * single_best/failover：仅链路0 一份（对照云端 dup 统计应归零） */
                bool dual = DupSelController::getInstance().effectiveMode() == VWD_MODE_DUAL_DUP;
                ::sendto(fds[0], pkt.data(), pkt.size(), 0, (sockaddr *)&dst[0], sizeof(dst[0]));
                if (dual) {
                    h->flags = VWD_F_DUPLICATE;
                    ::sendto(fds[1], pkt.data(), pkt.size(), 0, (sockaddr *)&dst[1], sizeof(dst[1]));
                    h->flags = 0;
                    up_sent.fetch_add(2);
                } else {
                    up_sent.fetch_add(1);
                }
            } else {
                /* kernel：eBPF 克隆副本；none：单发对照 */
                ::sendto(fds[0], pkt.data(), pkt.size(), 0, (sockaddr *)&dst[0], sizeof(dst[0]));
                up_sent.fetch_add(1);
            }
        }
        g_run.store(false);
        for (auto &t : rx) if (t.joinable()) t.join();
        fprintf(stderr, "[up] done: sent=%llu (逻辑包 seq_max=%u)\n",
                (unsigned long long)up_sent.load(), seq);
    }
cleanup:
    for (int fd : fds) close(fd);
}

/* 纯下行模式（P2）：自身监听每链路 local_port+100，周期发 DATA_UP"端点注册"包
 * （flow_id=0xFEED），云端据此把下行发到这些 socket。 */
static void downlink_only(const TestCfg &tc, DedupEngine &dedup, int duration_s) {
    std::vector<int> fds;
    for (size_t i = 0; i < tc.links.size(); ++i) {
        uint16_t port = (uint16_t)(tc.links[i].local_port ? tc.links[i].local_port + 100 : 39900 + i);
        int fd = open_udp_sock(tc.links[i].ifname, port);
        if (fd < 0) continue;
        fds.push_back(fd);
    }
    if (fds.empty()) { fprintf(stderr, "[down] no rx sockets\n"); return; }
    /* 先设 200ms 收包超时，避免首个 recv 无包时永久阻塞 */
    for (int fd : fds) {
        timeval tv{0, 200 * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    auto end_at = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);
    auto next_reg = std::chrono::steady_clock::now();
    uint32_t reg_seq = 0;
    while (g_run.load() && std::chrono::steady_clock::now() < end_at) {
        /* 周期端点注册（seq 递增，云端按唯一包统计，不污染 dup 计数） */
        if (std::chrono::steady_clock::now() >= next_reg) {
            next_reg += std::chrono::seconds(2);
            for (size_t i = 0; i < fds.size(); ++i) {
                uint8_t reg[VWD_HDR_SIZE + 16];
                memset(reg, 0, sizeof(reg));
                VwPktHdr *h = (VwPktHdr *)reg;
                vwd_init_hdr(h, VWD_T_DATA_UP, (uint8_t)i, 0);
                h->flow_id = 0xFEEDu;
                h->seq = ++reg_seq;
                h->t_send_ns = 0;
                sockaddr_in da;
                memset(&da, 0, sizeof(da));
                da.sin_family = AF_INET;
                da.sin_port = htons(tc.links[i].anchor_port);
                da.sin_addr.s_addr = inet_addr(tc.links[i].anchor_ip.c_str());
                ::sendto(fds[i], reg, sizeof(reg), 0, (sockaddr *)&da, sizeof(da));
            }
        }
        for (size_t i = 0; i < fds.size(); ++i) {
            uint8_t buf[2048];
            /* 首包等待（200ms 超时），命中后 MSG_DONTWAIT 排空——
             * 否则每轮仅收 1 包（~5pps 排空率 < 50pps 入流）导致内核缓冲溢出丢包 */
            sockaddr_in from;
            socklen_t flen = sizeof(from);
            int n = ::recvfrom(fds[i], buf, sizeof(buf), 0, (sockaddr *)&from, &flen);
            while (n > 0) {
                const VwPktHdr *h = vwd_parse(buf, (size_t)n);
                if (h && h->type == VWD_T_DATA_DOWN) {
                    DedupEngine::Stats delta;
                    dedup.feed(h->flow_id, h->seq, (int)i, delta);
                }
                n = ::recvfrom(fds[i], buf, sizeof(buf), MSG_DONTWAIT, (sockaddr *)&from, &flen);
            }
        }
    }
    for (int fd : fds) close(fd);
}

/* ---------------- 周期统计打印 ---------------- */
static void stats_printer(DedupEngine *dedup, int period_s) {
    auto next_t = std::chrono::steady_clock::now();
    while (g_run.load()) {
        next_t += std::chrono::seconds(period_s);
        std::this_thread::sleep_until(next_t);

        /* 链路质量 */
        printf("[links] ");
        for (auto &q : LinkMonitor::getInstance().snapshot()) {
            printf("%s: alive=%d rtt=%.1fms loss=%.1f%% bw=%.0fkbps | ", q.name.c_str(),
                   (int)q.alive, q.rtt_ewma_ms < 0 ? -1.0 : q.rtt_ewma_ms, q.loss_pct,
                   q.bw_ewma_kbps < 0 ? -1.0 : q.bw_ewma_kbps);
        }
        printf(" mode=%s\n", DupSelController::getInstance().modeName());

        /* 云端对账：向锚点0发 STATS_REQ，打回执 */
        if (dedup != nullptr) {
            auto s = dedup->stats();
            printf("[down] rx=%llu unique=%llu dup=%llu first0=%llu first1=%llu oos=%llu last_seq=%llu\n",
                   (unsigned long long)s.rx_total, (unsigned long long)s.rx_unique,
                   (unsigned long long)s.rx_dup, (unsigned long long)s.first_by_link[0],
                   (unsigned long long)s.first_by_link[1], (unsigned long long)s.oos_events,
                   (unsigned long long)s.last_seq);
        }
    }
}

int main(int argc, char **argv) {
    std::string cfg_path = "conf/config.json";
    std::string mode = "full";
    std::string dup = "user"; /* kernel | user | none */
    double rate_pps = 100.0;
    int pkt_bytes = 256;
    int duration_s = 60;
    uint32_t flow_id = 0x5100u;

    static option longopts[] = {
        {"config", required_argument, 0, 'c'},
        {"mode", required_argument, 0, 'm'},
        {"dup", required_argument, 0, 'd'},
        {"rate", required_argument, 0, 'r'},
        {"size", required_argument, 0, 's'},
        {"dur", required_argument, 0, 'T'},
        {"flow", required_argument, 0, 'f'},
        {0, 0, 0, 0}};
    int opt;
    while ((opt = getopt_long(argc, argv, "c:m:d:r:s:T:f:", longopts, nullptr)) != -1) {
        switch (opt) {
            case 'c': cfg_path = optarg; break;
            case 'm': mode = optarg; break;
            case 'd': dup = optarg; break;
            case 'r': rate_pps = atof(optarg); break;
            case 's': pkt_bytes = atoi(optarg); break;
            case 'T': duration_s = atoi(optarg); break;
            case 'f': flow_id = (uint32_t)strtoul(optarg, nullptr, 0); break;
            default: return 2;
        }
    }

    TestCfg tc = load_cfg(cfg_path);
    if (tc.links.empty()) {
        fprintf(stderr, "no links in '%s' (Vwise.DupSel.Links). See 实施文档.\n", cfg_path.c_str());
        return 2;
    }

    UpDup up_dup = UpDup::USER;
    if (dup == "kernel") up_dup = UpDup::KERNEL;
    else if (dup == "none") up_dup = UpDup::NONE;

    /* Ctrl+C 优雅退出 */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = [](int) { g_run.store(false); };
        sigaction(SIGINT, &sa, nullptr);
    }

    /* eBPF 加载（失败自动降级 user 模式，由 controller 报告） */
    static EbpfLoader ebpf;
    if (up_dup == UpDup::KERNEL) {
        std::vector<std::string> nics = tc.ebpf_nics;
        if (nics.empty()) {
            for (auto &lc : tc.links) if (!lc.ifname.empty()) nics.push_back(lc.ifname);
        }
        if (tc.ebpf_enable && !ebpf.load(tc.ebpf_obj, nics)) {
            fprintf(stderr, "[main] eBPF load failed -> fallback to user dup\n");
            up_dup = UpDup::USER;
        }
    }

    /* 去重引擎（须在控制器 start 前声明并挂接，避免上报线程读到悬挂指针） */
    DedupEngine dedup;

    /* DupSelController（探测驱动模式状态机 + 状态打印 + v2 看板上报） */
    DupSelController::getInstance().attachDedup(&dedup);
    DupSelController::getInstance().start(up_dup == UpDup::KERNEL ? &ebpf : nullptr, tc.th);

    /* LinkMonitor */
    if (!LinkMonitor::getInstance().start(tc.links, tc.probe)) {
        fprintf(stderr, "LinkMonitor start failed\n");
        return 1;
    }
    /* v2：CTRL 指令（看板 term_mode/train）经探测 socket 送达 → 状态机处理 */
    LinkMonitor::getInstance().setCtrlHandler([](const std::string &json) {
        return DupSelController::getInstance().onCommand(json);
    });

    /* 去重统计线程（dedup 已在控制器挂接） */
    std::thread stats_th(stats_printer, &dedup, 2);

    /* 业务线程 */
    SessionCfg sc{&tc, up_dup, rate_pps, pkt_bytes, duration_s, flow_id, nullptr};
    std::vector<std::thread> workers;
    if (mode == "probe") {
        /* 仅探测：无业务线程，用等待线程维持 --dur 时长后退出 */
        workers.emplace_back([duration_s]() {
            auto end = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);
            while (g_run.load() && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            g_run.store(false);
        });
    } else if (mode == "down") {
        sc.dedup = &dedup;
        workers.emplace_back(downlink_only, std::cref(tc), std::ref(dedup), duration_s);
    } else if (mode == "up") {
        workers.emplace_back(session_updown, std::cref(sc));
    } else { /* full：上行双发 + 下行接收共享同一批 socket */
        sc.dedup = &dedup;
        workers.emplace_back(session_updown, std::cref(sc));
    }

    for (auto &w : workers) {
        if (w.joinable()) w.join();
    }
    g_run.store(false);
    if (stats_th.joinable()) stats_th.join();

    /* 退出前发送 BYE 标记的 TERM_STATS：云端立即判离线并冻结验收计数，
     * 同时闭合"2s 快照滞后"——分子包含完整测试窗口 */
    {
        uint8_t pkt[VWD_TERM_STATS_SIZE];
        size_t len = 0;
        if (DupSelController::getInstance().buildTermStats(pkt, sizeof(pkt), len)) {
            ((VwPktHdr *)pkt)->flags = VWD_F_TERM_BYE;
            LinkMonitor::getInstance().sendToLink(0, pkt, len);
        }
    }

    LinkMonitor::getInstance().stop();
    DupSelController::getInstance().stop();
    return 0;
}
