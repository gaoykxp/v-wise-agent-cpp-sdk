/**********************************************************************************************************************
    > File: vwise_dup_protocol.h
    > 双链路远控 共享线协议（云端 v-wise-cloud-core 与 车端 v-wise-agent-cpp-sdk 必须保持字节一致）
    > 维护规则：本文件与 v-wise-agent-cpp-sdk/include/vwise_dup_protocol.h 内容完全相同，
    >           任何修改必须同步两份拷贝，scripts/check_protocol_sync.sh 用于校验。
    > 字节序：小端（双端均为 x86/ARM 小端平台，不做跨端转换）。
**********************************************************************************************************************/
#ifndef VWISE_DUP_PROTOCOL_H
#define VWISE_DUP_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VWD_MAGIC        0x31445756u  /* "VWD1" 小端 */
#define VWD_VERSION      2            /* v2: 新增 TERM_STATS/CTRL（Web 看板支撑） */

/* eBPF 过滤用 socket mark 位（车端内核态与用户态共用） */
#define VWD_MARK_RC      0x100u       /* 远控流：需要双发选收处理 */
#define VWD_MARK_CLONED  0x200u       /* 已克隆标记（eBPF 防复制风暴） */

/* 报文类型 */
enum {
    VWD_T_PROBE_REQ = 1,   /* 终端→云：链路探测（RTT / 丢包 / 带宽 train） */
    VWD_T_PROBE_ACK = 2,   /* 云→终端：探测回应（回显 t_send_ns；train 末包携带云侧 rx_rate） */
    VWD_T_DATA_UP   = 10,  /* 终端→云：上行数据（双发副本，云侧按 (flow_id,seq) 去重先到先得） */
    VWD_T_DATA_DOWN = 20,  /* 云→终端：下行数据（择优/双发，终端按 (flow_id,seq) 去重） */
    VWD_T_STATS_REQ = 30,  /* 终端→云：请求云侧瞬时统计（测试对账用） */
    VWD_T_STATS_ACK = 31,  /* 云→终端：回传云侧统计计数器 */
    /* ---- v2：Web 看板支撑（无 broker 环境的终端状态上报 / 控制下发） ---- */
    VWD_T_TERM_STATS = 40, /* 终端→云：周期(2s)终端质量/去重上报（无 MQTT 时看板数据源） */
    VWD_T_CTRL       = 41, /* 云→终端：控制指令（JSON 文本，模式切换/train 触发） */
};

/* flags 位 */
#define VWD_F_TRAIN_END 0x01   /* PROBE_REQ：带宽 train 的最后一个包，云侧回 ACK 携带 rx_rate */
#define VWD_F_DUPLICATE 0x02   /* 用户态双发时第二份副本置位（内核 eBPF 双发不置位，两端均不依赖此判定重复） */
#define VWD_F_TERM_BYE  0x04   /* TERM_STATS：终端退出标记——云侧立即判离线并冻结计数（验收窗口闭合） */

/* 长期探测/数据模式（DupSelController 状态机，双端语义一致） */
#define VWD_MODE_SINGLE_BEST 0 /* 单发择优：不复制，走优链路 */
#define VWD_MODE_DUAL_DUP    1 /* 双发：双链路复制，云端先到先得 */
#define VWD_MODE_FAILOVER    2 /* 故障切换：强制走幸存链路（mode 字段 + forced_link 指明链路） */

#pragma pack(push, 1)
typedef struct VwPktHdr_ {
    uint32_t magic;       /* VWD_MAGIC */
    uint8_t  version;     /* VWD_VERSION */
    uint8_t  type;        /* VWD_T_* */
    uint8_t  link_id;      /* 发送链路编号（云侧统计按接收锚点独立进行，不依赖此字段） */
    uint8_t  flags;       /* VWD_F_* */
    uint32_t flow_id;     /* 流标识（去重键之一） */
    uint32_t seq;         /* 流内递增序号（去重键，双发副本序号相同） */
    uint64_t t_send_ns;   /* 发送端单调时钟时间戳 */
    uint64_t t_recv_ns;   /* 接收端时间戳（回应包回填；请求包填 0） */
} VwPktHdr;

typedef struct VwProbeExtra_ {  /* PROBE_REQ / PROBE_ACK 公共净荷区 */
    uint32_t train_id;      /* train 编号（0=常规单包探测，非0=带宽train） */
    uint32_t rx_rate_kbps;  /* ACK 回填：云侧 train 实测接收速率（常规 ACK 为 0） */
    uint32_t train_bytes;   /* ACK 回填：train 总接收字节数 */
    uint32_t rsv;
} VwProbeExtra;

typedef struct VwStatsExtra_ {  /* STATS_REQ / STATS_ACK 净荷区 */
    uint64_t f_u64[4];      /* ACK: [0]=云侧去重后unique总数 [1]=重复丢弃总数
                              [2]=锚点0先到数 [3]=锚点1先到数；REQ 侧填 0 */
} VwStatsExtra;

/* ---- v2：TERM_STATS 净荷（终端周期上报，布局固定，双端一致） ---- */
#define VWD_TERM_NO_SAMPLE 0xFFFFFFFFu

typedef struct VwTermStatsLink_ {  /* 单链路上报项 */
    uint32_t id;
    uint32_t alive;         /* 0/1 */
    uint32_t rtt_us;        /* RTT 微秒；VWD_TERM_NO_SAMPLE=无样本 */
    uint32_t loss_pct_x100; /* 丢包率×100 */
    uint32_t bw_kbps;       /* 云侧实测 train 速率；VWD_TERM_NO_SAMPLE=无样本 */
    uint32_t probes_sent;
    uint32_t probes_acked;
} VwTermStatsLink;

typedef struct VwTermStatsExtra_ {
    uint32_t mode;          /* VWD_MODE_* */
    uint32_t ebpf_on;       /* 0=用户态降级 1=内核态 */
    uint32_t dedup_rx;
    uint32_t dedup_unique;
    uint32_t dedup_dup;
    uint32_t dedup_oos;
    uint32_t dedup_last_seq;
    uint32_t nlinks;        /* links[] 有效数（1..2） */
    VwTermStatsLink links[2];
} VwTermStatsExtra;

#define VWD_TERM_STATS_SIZE  (sizeof(VwPktHdr) + sizeof(VwTermStatsExtra))
#define VWD_CTRL_MAX         480  /* CTRL 净荷（JSON 文本）最大长度 */
#pragma pack(pop)

#define VWD_HDR_SIZE      sizeof(VwPktHdr)      /* 32 */
#define VWD_PROBE_SIZE    (sizeof(VwPktHdr) + sizeof(VwProbeExtra))
#define VWD_STATS_SIZE    (sizeof(VwPktHdr) + sizeof(VwStatsExtra))

/* 常规探测包总长（含填充），带宽 train 包总长 */
#define VWD_PROBE_MIN_BYTES    64
#define VWD_PROBE_TRAIN_BYTES  1200

/* 常规探测包填充长度（保证总长 ≥ VWD_PROBE_MIN_BYTES） */
#define VWD_PROBE_PAD_BYTES    (VWD_PROBE_MIN_BYTES - VWD_PROBE_SIZE)

/* 去重滑动窗口槽数（2 的幂；乱序容忍窗口 = 槽数 - 1） */
#define VWD_DEDUP_RING_BITS  10
#define VWD_DEDUP_RING_SIZE  (1u << VWD_DEDUP_RING_BITS)

static inline int vwd_pkt_type_valid(uint8_t t) {
    return t == VWD_T_PROBE_REQ || t == VWD_T_PROBE_ACK || t == VWD_T_DATA_UP ||
           t == VWD_T_DATA_DOWN || t == VWD_T_STATS_REQ || t == VWD_T_STATS_ACK ||
           t == VWD_T_TERM_STATS || t == VWD_T_CTRL;
}

/* 解析收包：校验 magic/version，返回头部指针；不合法返回 NULL。 */
static inline const VwPktHdr *vwd_parse(const void *buf, size_t len) {
    if (buf == NULL || len < VWD_HDR_SIZE) return NULL;
    const VwPktHdr *h = (const VwPktHdr *)buf;
    if (h->magic != VWD_MAGIC || h->version != VWD_VERSION) return NULL;
    if (!vwd_pkt_type_valid(h->type)) return NULL;
    return h;
}

/* 初始化头部（type/flow_id/seq/t_send_ns 由调用方后续填充） */
static inline void vwd_init_hdr(VwPktHdr *h, uint8_t type, uint8_t link_id, uint8_t flags) {
    memset(h, 0, sizeof(*h));
    h->magic = VWD_MAGIC;
    h->version = VWD_VERSION;
    h->type = type;
    h->link_id = link_id;
    h->flags = flags;
}

#ifdef __cplusplus
}
#endif

#endif /* VWISE_DUP_PROTOCOL_H */
