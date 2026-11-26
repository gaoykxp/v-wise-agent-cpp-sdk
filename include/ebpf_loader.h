/**********************************************************************************************************************
    > File: ebpf_loader.h
    > libbpf 加载器（P1）：加载 vwise_dup.bpf.o，attach 到两张网卡的 clsact egress，
    * 维护 nic_ctrl_map（mode 热切换 / peer ifindex）。
    * 未启用/未加载时：loaded()=false，DupSelController 自动进入用户态降级模式。
    * 依赖：libbpf >= 1.0（Ubuntu 22.04 apt 为 0.5，需源码编译 ≥1.0，见实施文档）；
    *       缺失时 CMake 关闭 VWISE_EBPF，本文件退化为空实现。
**********************************************************************************************************************/
#ifndef TANGO_NEXUS_EBPF_LOADER_H
#define TANGO_NEXUS_EBPF_LOADER_H

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cmsr {
namespace vwise {

class EbpfLoader {
public:
    EbpfLoader() = default;
    ~EbpfLoader() { unload(); }

    /* obj_path: vwise_dup.bpf.o；nics: 参与双发的网卡名列表（成对/≥2 时配对克隆，
     * 单元素时该网卡仅按 map 单独控制） */
    bool load(const std::string &obj_path, const std::vector<std::string> &nics);
    void unload();

    bool loaded() const { return loaded_; }

    /* DUAL_DUP 开/关：更新所有已 attach 网卡的 mode */
    bool set_dup_enabled(bool en);

    /* 单网卡粒度控制 */
    bool set_nic_mode(const std::string &nic, int mode, int peer_ifindex);

    size_t attached_count() const { return attached_ifindexes_.size(); }

private:
    bool attach_to_nic(const std::string &nic, uint32_t ifindex);
    bool update_ctrl_map(uint32_t ifindex, int mode, uint32_t peer_ifindex);

    void *obj_ = nullptr;            /* struct bpf_object*（void* 避免头污染业务层） */
    void *tc_hook_ = nullptr;        /* struct bpf_tc_hook* 保留 per-nic */
    int prog_fd_ = -1;
    int ctrl_map_fd_ = -1;
    bool loaded_ = false;
    std::vector<std::string> nics_;
    std::vector<uint32_t> attached_ifindexes_;
    /* ifindex → 本网卡 MAC（写入 ctrl map，供 BPF 重写克隆帧源 MAC） */
    std::map<uint32_t, std::array<uint8_t, 6>> nic_macs_;
};

} // namespace vwise
} // namespace cmsr

#endif // TANGO_NEXUS_EBPF_LOADER_H
