#include "diag_event_bus.h"

#include "base_timer.h"
#include "log.h"

namespace vwise::modem {

// ---- DiagEvent 元数据（名称/责任域），集中维护 ----

const char* diagEventName(DiagEvent ev) {
    switch (ev) {
        case DiagEvent::ModuleBoot:      return "module_boot";
        case DiagEvent::ModulePowerDown: return "module_powerdown";
        case DiagEvent::SimRemoved:      return "sim_removed";
        case DiagEvent::SimInserted:     return "sim_inserted";
        case DiagEvent::NetDenied:       return "net_denied";
        case DiagEvent::NetNwDeact:      return "net_nw_deact";
        case DiagEvent::NetNwDetach:     return "net_nw_detach";
        case DiagEvent::NetMeDeact:      return "net_me_deact";
        case DiagEvent::NetMeDetach:     return "net_me_detach";
        case DiagEvent::PdnAct:          return "pdn_act";
        case DiagEvent::PdnDeact:        return "pdn_deact";
        case DiagEvent::RmNetDown:       return "rmnet_down";
        case DiagEvent::RatChange:       return "rat_change";
    }
    return "unknown";
}

const char* diagEventDomain(DiagEvent ev) {
    switch (ev) {
        case DiagEvent::ModuleBoot:
        case DiagEvent::ModulePowerDown:
        case DiagEvent::NetMeDeact:
        case DiagEvent::NetMeDetach:
            return "module";
        case DiagEvent::SimRemoved:
        case DiagEvent::SimInserted:
            return "sim";
        case DiagEvent::NetDenied:
        case DiagEvent::NetNwDeact:
        case DiagEvent::NetNwDetach:
        case DiagEvent::RatChange:
            return "network";
        case DiagEvent::RmNetDown:
            return "host";
        case DiagEvent::PdnAct:
        case DiagEvent::PdnDeact:
            return "";   // 中性事件，责任域看上下文
    }
    return "";
}

// ---- 事件总线 ----

DiagEventBus& DiagEventBus::getInstance() {
    static DiagEventBus inst;
    return inst;
}

void DiagEventBus::publish(DiagEvent ev, std::string raw) {
    DiagEventRecord rec;
    rec.ts_ms = base_tools::BaseTimer::GetMilliTime();
    rec.type = ev;
    rec.domain = diagEventDomain(ev);
    rec.raw = std::move(raw);

    LogInfo << "[DiagEvent] " << diagEventName(ev) << " (" << rec.domain << "): " << rec.raw;

    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (ring_.size() >= kRingCapacity)
            ring_.pop_front();
        ring_.push_back(std::move(rec));
    }
}

std::vector<DiagEventRecord> DiagEventBus::recent(size_t n) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const size_t count = (n < ring_.size()) ? n : ring_.size();
    return { ring_.end() - count, ring_.end() };
}

size_t DiagEventBus::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return ring_.size();
}

} // namespace vwise::modem
