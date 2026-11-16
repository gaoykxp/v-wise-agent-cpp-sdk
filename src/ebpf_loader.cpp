/**********************************************************************************************************************
    > File: ebpf_loader.cpp
    * libbpf >= 1.0 实现；未定义 VWISE_EBPF 时编译为空实现（降级路径）。
    * 日志：fprintf（零依赖，v_wise_agent 与 vwise_duptest 两个目标均可链接）。
**********************************************************************************************************************/
#include "ebpf_loader.h"
#include <cstdio>
#include <string>

#ifdef VWISE_EBPF
#include "vwise_dup_protocol.h"   /* VWD_MODE_* 常量 */
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <net/if.h>
#include <cerrno>
#include <cstring>

namespace cmsr {
namespace vwise {

static int nic_ifindex(const std::string &nic) {
    return (int)if_nametoindex(nic.c_str());
}

bool EbpfLoader::load(const std::string &obj_path, const std::vector<std::string> &nics) {
    if (nics.size() < 2) {
        fprintf(stderr, "[ebpf] need >=2 nics for dual-send, got %zu\n", nics.size());
        return false;
    }

    struct bpf_object *obj = bpf_object__open_file(obj_path.c_str(), nullptr);
    if (!obj) {
        fprintf(stderr, "[ebpf] open %s failed\n", obj_path.c_str());
        return false;
    }
    if (bpf_object__load(obj) != 0) {
        fprintf(stderr, "[ebpf] load failed (verifier denied or missing kernel BTF)\n");
        bpf_object__close(obj);
        return false;
    }
    struct bpf_program *prog = bpf_object__find_program_by_name(obj, "vwd_egress_dup");
    if (!prog) {
        fprintf(stderr, "[ebpf] program vwd_egress_dup not found\n");
        bpf_object__close(obj);
        return false;
    }
    struct bpf_map *m = bpf_object__find_map_by_name(obj, "nic_ctrl_map");
    if (!m) {
        fprintf(stderr, "[ebpf] map nic_ctrl_map not found\n");
        bpf_object__close(obj);
        return false;
    }

    obj_ = obj;
    prog_fd_ = bpf_program__fd(prog);
    ctrl_map_fd_ = bpf_map__fd(m);
    nics_ = nics;

    /* 依次 attach 每张网卡，peer = 列表中相邻网卡（0↔1） */
    for (size_t i = 0; i < nics.size(); ++i) {
        uint32_t ifidx = (uint32_t)nic_ifindex(nics[i]);
        if (ifidx == 0) {
            fprintf(stderr, "[ebpf] nic %s not found, skipped\n", nics[i].c_str());
            continue;
        }
        size_t peer_pos = (i == 0) ? nics.size() - 1 : i - 1;
        uint32_t peer_ifidx = (uint32_t)nic_ifindex(nics[peer_pos]);
        if (peer_ifidx == 0) {
            fprintf(stderr, "[ebpf] peer nic of %s not found, skipped\n", nics[i].c_str());
            continue;
        }
        if (!attach_to_nic(nics[i], ifidx)) continue;
        attached_ifindexes_.push_back(ifidx);
        /* 初始 mode=SINGLE_BEST（不复制），由 DupSelController 热切换 */
        update_ctrl_map(ifidx, VWD_MODE_SINGLE_BEST, peer_ifidx);
    }

    if (attached_ifindexes_.size() < 2) {
        fprintf(stderr, "[ebpf] only %zu nics attached, need 2; user-space dup fallback\n",
                attached_ifindexes_.size());
        unload();
        return false;
    }
    loaded_ = true;
    fprintf(stderr, "[ebpf] loaded, attached %zu nics\n", attached_ifindexes_.size());
    return true;
}

bool EbpfLoader::attach_to_nic(const std::string &nic, uint32_t ifindex) {
    DECLARE_LIBBPF_OPTS(bpf_tc_hook, hook, .ifindex = (int)ifindex,
                        .attach_point = BPF_TC_EGRESS);
    /* 创建 clsact qdisc（已存在时 EEXIST 视为成功） */
    int err = bpf_tc_hook_create(&hook);
    if (err != 0 && err != -EEXIST) {
        fprintf(stderr, "[ebpf] clsact create failed on %s err=%d\n", nic.c_str(), err);
        return false;
    }
    DECLARE_LIBBPF_OPTS(bpf_tc_opts, attach_opts, .prog_fd = prog_fd_, .handle = 1,
                        .priority = 1);
    err = bpf_tc_attach(&hook, &attach_opts);
    if (err != 0) {
        fprintf(stderr, "[ebpf] tc attach failed on %s err=%d\n", nic.c_str(), err);
        return false;
    }
    fprintf(stderr, "[ebpf] attached tc/egress on %s (ifindex=%u)\n", nic.c_str(),
            (unsigned)ifindex);
    return true;
}

bool EbpfLoader::update_ctrl_map(uint32_t ifindex, int mode, uint32_t peer_ifindex) {
    struct {
        uint32_t mode;
        uint32_t peer_ifindex;
    } val = {(uint32_t)mode, peer_ifindex};
    uint32_t key = ifindex;
    return bpf_map_update_elem(ctrl_map_fd_, &key, &val, BPF_ANY) == 0;
}

bool EbpfLoader::set_nic_mode(const std::string &nic, int mode, int peer_ifindex) {
    if (!loaded_) return false;
    uint32_t ifidx = (uint32_t)nic_ifindex(nic);
    if (ifidx == 0) return false;
    if (peer_ifindex < 0) {
        for (size_t i = 0; i < nics_.size(); ++i) {
            if (nics_[i] == nic) {
                size_t peer_pos = (i == 0) ? nics_.size() - 1 : i - 1;
                peer_ifindex = nic_ifindex(nics_[peer_pos]);
            }
        }
    }
    return update_ctrl_map(ifidx, mode, (uint32_t)peer_ifindex);
}

bool EbpfLoader::set_dup_enabled(bool en) {
    if (!loaded_) return false;
    bool ok = true;
    for (size_t i = 0; i < nics_.size(); ++i) {
        uint32_t ifidx = (uint32_t)nic_ifindex(nics_[i]);
        if (ifidx == 0) { ok = false; continue; }
        size_t peer_pos = (i == 0) ? nics_.size() - 1 : i - 1;
        uint32_t peer_ifidx = (uint32_t)nic_ifindex(nics_[peer_pos]);
        if (!update_ctrl_map(ifidx, en ? VWD_MODE_DUAL_DUP : VWD_MODE_SINGLE_BEST, peer_ifidx))
            ok = false;
    }
    return ok;
}

void EbpfLoader::unload() {
    if (obj_) {
        bpf_object__close((struct bpf_object *)obj_);
        obj_ = nullptr;
    }
    prog_fd_ = -1;
    ctrl_map_fd_ = -1;
    loaded_ = false;
    attached_ifindexes_.clear();
}

} // namespace vwise
} // namespace cmsr

#else /* =================== 未启用 eBPF：空实现（降级路径） =================== */

namespace cmsr {
namespace vwise {

bool EbpfLoader::load(const std::string &, const std::vector<std::string> &nics) {
    (void)nics;
    fprintf(stderr, "[ebpf] built without VWISE_EBPF (libbpf not found) — user-space dup fallback\n");
    return false;
}
void EbpfLoader::unload() {}
bool EbpfLoader::loaded() const { return false; }
bool EbpfLoader::set_dup_enabled(bool) { return false; }
bool EbpfLoader::set_nic_mode(const std::string &, int, int) { return false; }
size_t EbpfLoader::attached_count() const { return 0; }

} // namespace vwise
} // namespace cmsr

#endif /* VWISE_EBPF */
