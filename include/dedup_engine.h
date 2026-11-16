/**********************************************************************************************************************
    > File: dedup_engine.h
    > 终端下行去重引擎（P2）：云端择优/双发下行时，两路副本按 (flow_id, seq) 去重。
    * 算法与云端 dedup.h 一致（环形槽位表）；线程模型：外部持锁。
**********************************************************************************************************************/
#ifndef TANGO_NEXUS_DEDUP_ENGINE_H
#define TANGO_NEXUS_DEDUP_ENGINE_H

#include "vwise_dup_protocol.h"
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cmsr {
namespace vwise {

class DedupEngine {
public:
    struct Stats {
        uint64_t rx_total = 0;     /* 收到的所有下行包 */
        uint64_t rx_unique = 0;    /* 去重后唯一 */
        uint64_t rx_dup = 0;       /* 重复丢弃 */
        uint64_t first_by_link[8] = {0}; /* 每链路先到计数（终端按接收 socket/link 归属） */
        uint64_t last_seq = 0;     /* 最大已见 seq */
        uint64_t oos_events = 0;   /* 乱序事件（seq 落后于已见最大 seq） */
    };

    /* 喂入一个下行包：dup=true 表示重复已丢弃 */
    bool feed(uint32_t flow_id, uint32_t seq, int link_id, Stats &delta);

    Stats stats() const;
    void resetStats();

private:
    struct Slot {
        uint32_t seq = 0;
        bool used = false;
    };
    struct FlowState {
        std::vector<Slot> ring;
        FlowState() : ring(VWD_DEDUP_RING_SIZE) {}
    };
    std::unordered_map<uint32_t, FlowState> flows_;
    mutable std::mutex mtx_;
    Stats stats_;
};

} // namespace vwise
} // namespace cmsr

#endif // TANGO_NEXUS_DEDUP_ENGINE_H
