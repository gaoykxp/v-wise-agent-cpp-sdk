/**********************************************************************************************************************
    > File: dedup_engine.cpp
**********************************************************************************************************************/
#include "dedup_engine.h"

namespace cmsr {
namespace vwise {

bool DedupEngine::feed(uint32_t flow_id, uint32_t seq, int link_id, Stats &delta) {
    std::lock_guard<std::mutex> lk(mtx_);
    delta = Stats{};

    FlowState &f = flows_[flow_id];
    Slot &s = f.ring[seq & (VWD_DEDUP_RING_SIZE - 1)];
    bool dup = (s.used && s.seq == seq);
    if (!dup) {
        s.used = true;
        s.seq = seq;
    }

    stats_.rx_total++;
    if (dup) {
        stats_.rx_dup++;
    } else {
        stats_.rx_unique++;
        if ((uint64_t)seq >= stats_.last_seq) {
            stats_.last_seq = seq;
        } else {
            stats_.oos_events++;
        }
        if (link_id >= 0 && link_id < 8) stats_.first_by_link[link_id]++;
    }

    /* 返回本包增量（供调用方打印） */
    delta.rx_total = 1;
    delta.rx_dup = dup ? 1 : 0;
    delta.rx_unique = dup ? 0 : 1;
    if (!dup && link_id >= 0 && link_id < 8) delta.first_by_link[link_id] = 1;
    delta.last_seq = stats_.last_seq;
    return dup;
}

DedupEngine::Stats DedupEngine::stats() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return stats_;
}

void DedupEngine::resetStats() {
    std::lock_guard<std::mutex> lk(mtx_);
    stats_ = Stats{};
}

} // namespace vwise
} // namespace cmsr
