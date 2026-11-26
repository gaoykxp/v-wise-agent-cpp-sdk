/* SPDX-License-Identifier: GPL-2.0
 * File: vwise_dup.bpf.c
 * 双发选收 eBPF 程序（设计稿 3.2/5 P1）：
 *   - attach 于两张网卡(clsact)的 tc egress；
 *   - 过滤：仅远控流（skb->mark & VWD_MARK_RC）；
 *   - 防复制风暴：先把 skb->mark 置上 CLONED 位再 clone_redirect，
 *     副本到对端网卡 egress 时命中 CLONED 直接放行，不再克隆；
 *   - 模式控制：nic_ctrl_map（key=本网卡 ifindex）→ {mode, peer_ifindex}，
 *     mode != DUAL_DUP 时克隆旁路（SDK 用户态热切换）；
 *   - 编译：clang -target bpf -O2 -c vwise_dup.bpf.c -o vwise_dup.bpf.o
 *     （需 libbpf-dev 提供的 bpf_helpers.h；Ubuntu 22.04 RPi 直接可用）
 */
#include <linux/bpf.h>
#include <linux/pkt_cls.h>
#include <bpf/bpf_helpers.h>

#ifndef VWD_MARK_RC
#define VWD_MARK_RC     0x100
#endif
#ifndef VWD_MARK_CLONED
#define VWD_MARK_CLONED 0x200
#endif
#ifndef VWD_MODE_DUAL_DUP
#define VWD_MODE_DUAL_DUP 1
#endif

struct nic_ctrl {
    __u32 mode;         /* VWD_MODE_* */
    __u32 peer_ifindex; /* 对端 DSDA 网卡 ifindex */
    __u32 _pad;
    __u8  own_mac[6];   /* 本网卡 MAC：克隆帧源 MAC 重写用 */
};

/* 每网卡控制项：key = 本网卡 ifindex */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, struct nic_ctrl);
} nic_ctrl_map SEC(".maps");

SEC("tc")
int vwd_egress_dup(struct __sk_buff *skb)
{
    __u32 mark = skb->mark;

    /* 1) 流过滤：仅远控流参与双发 */
    if (!(mark & VWD_MARK_RC))
        return TC_ACT_OK;

    /* 2) 防复制风暴：已克隆的副本直接放行（对端 egress 再入点）。
     *    clone_redirect 复用原网卡的 L2 头（src MAC = 原网卡 MAC），
     *    同 L2 双口场景（两网卡同网段经同一 AP 网桥）会触发 AP 的 MAC 漂移
     *    保护丢弃——重写源 MAC 为本网卡 MAC 后再放行 */
    if (mark & VWD_MARK_CLONED) {
        __u32 my_ifidx = skb->ifindex;
        struct nic_ctrl *me = bpf_map_lookup_elem(&nic_ctrl_map, &my_ifidx);
        if (me)
            bpf_skb_store_bytes(skb, 6 /* eth src mac */, me->own_mac, 6, 0);
        return TC_ACT_OK;
    }

    /* 3) 模式检查：SDK 用户态通过 map 热切换 */
    __u32 ifidx = skb->ifindex;
    struct nic_ctrl *c = bpf_map_lookup_elem(&nic_ctrl_map, &ifidx);
    if (!c || c->mode != VWD_MODE_DUAL_DUP)
        return TC_ACT_OK;

    /* 4) 先置 CLONED 位再克隆：原包与副本都带 CLONED。
     *    原包继续走本网卡出队，副本经 redirect 到对端网卡 egress，
     *    命中第 2 步重写源 MAC 后放行，杜绝二次克隆。 */
    skb->mark = mark | VWD_MARK_CLONED;
    bpf_clone_redirect(skb, c->peer_ifindex, 0 /* egress */);

    return TC_ACT_OK;
}

char LICENSE[] SEC("license") = "GPL";
