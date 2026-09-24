/**********************************************************************************************************************
    > File Name: cellular_diagnosis.cpp
    > Desc: 蜂窝诊断判定统一实现（接口与枚举语义说明见 cellular_diagnosis.h）
    >       本文件为唯一判定来源，勿在采集层（probe_manager 等）内联重写 error_code/
    >       reg_stat/ue_state 推导——修改枚举值或优先级只改这里。
**********************************************************************************************************************/

#include "cellular_diagnosis.h"

namespace cmsr {
    namespace vwise {

        // ==================== wireless.error_code ====================
        // 枚举（6 值）：NONE / SIM_FAULT / REGISTER_FAIL / NOT_REGISTERED /
        //              NO_CELL / WEAK_SIGNAL
        // 优先级：SIM_FAULT > REGISTER_FAIL > NOT_REGISTERED > NO_CELL > WEAK_SIGNAL > NONE
        // （SIM/显式被拒最特异；未注册是"无数据业务"的定论；无小区常是未注册的
        //   前置/伴随；弱信号时业务可能仍可用，排最后）
        std::string deriveWirelessErrorCode(const rpi::SimInfo &simInfo,
                                            const ::vwise::modem::DiagSnapshot &snap,
                                            int lteThr, int nrThr) {
            if (simInfo.status == rpi::SimStatus::ABSENT ||
                simInfo.status == rpi::SimStatus::LOCKED) {
                return "SIM_FAULT";
            }
            if (snap.reg.ps_stat == 3 || snap.reg.cs_stat == 3 || snap.reg.stat_5g == 3) {
                return "REGISTER_FAIL";
            }
            if (!snap.effective_registered) {
                return "NOT_REGISTERED";
            }
            if (!snap.cell.valid) {
                return "NO_CELL";
            }
            if (snap.signal.valid && snap.signal.rsrp != -999) {
                const int thr = (snap.signal.rat == "NR5G") ? nrThr : lteThr;
                if (thr > 0 && snap.signal.rsrp <= thr) return "WEAK_SIGNAL";
            }
            return "NONE";
        }

        // 回退路径：与驱动路径同枚举同优先级；technology 为空（查询失败）时
        // 按 LTE 阈值分档，与原内联逻辑一致
        std::string deriveWirelessErrorCodeFallback(
            const rpi::RPIModuleInterface::NetworkWirelessInfo &wirelessInfo,
            bool simFault, int lteThr, int nrThr) {
            const auto &signalInfo = wirelessInfo.signal;
            const auto &cellInfo = wirelessInfo.cell;
            if (simFault) {
                return "SIM_FAULT";
            }
            if (cellInfo.reg_stat == static_cast<int>(rpi::NetworkRegStatus::REGISTRATION_DENIED)) {
                return "REGISTER_FAIL";
            }
            const bool registered =
                cellInfo.reg_stat == static_cast<int>(rpi::NetworkRegStatus::REGISTERED_HOME) ||
                cellInfo.reg_stat == static_cast<int>(rpi::NetworkRegStatus::REGISTERED_ROAMING);
            if (!registered) {
                return "NOT_REGISTERED";
            }
            if (cellInfo.cell_id.empty()) {
                return "NO_CELL";
            }
            const int thr = (signalInfo.technology == "NR5G") ? nrThr : lteThr;
            if (thr > 0 && signalInfo.rsrp <= thr) return "WEAK_SIGNAL";
            return "NONE";
        }

        // ==================== 状态字符串映射 ====================
        const char *ueStateName(::vwise::modem::ServingCell::UeState s) {
            switch (s) {
                case ::vwise::modem::ServingCell::UeState::SEARCH:  return "SEARCH";
                case ::vwise::modem::ServingCell::UeState::LIMSRV:  return "LIMSRV";
                case ::vwise::modem::ServingCell::UeState::NOCONN:  return "NOCONN";
                case ::vwise::modem::ServingCell::UeState::CONNECT: return "CONNECT";
                default: return "UNKNOWN";
            }
        }

        std::string regStatString(const ::vwise::modem::DiagSnapshot &snap) {
            if (snap.reg.ps_stat == 3 || snap.reg.cs_stat == 3 || snap.reg.stat_5g == 3)
                return "REGISTRATION_DENIED";
            if (snap.effective_registered) {
                if (snap.reg.ps_stat == 5 || snap.reg.stat_5g == 5) return "REGISTERED_ROAMING";
                return "REGISTERED_HOME";
            }
            if (snap.reg.ps_stat == 2 || snap.reg.stat_5g == 2) return "SEARCHING";
            return "NOT_REGISTERED";
        }

        std::string regStatStringFallback(int regStatRaw) {
            switch (regStatRaw) {
                case static_cast<int>(rpi::NetworkRegStatus::NOT_REGISTERED):
                    return "NOT_REGISTERED";
                case static_cast<int>(rpi::NetworkRegStatus::REGISTERED_HOME):
                    return "REGISTERED_HOME";
                case static_cast<int>(rpi::NetworkRegStatus::SEARCHING):
                    return "SEARCHING";
                case static_cast<int>(rpi::NetworkRegStatus::REGISTRATION_DENIED):
                    return "REGISTRATION_DENIED";
                case static_cast<int>(rpi::NetworkRegStatus::REGISTERED_ROAMING):
                    return "REGISTERED_ROAMING";
                case static_cast<int>(rpi::NetworkRegStatus::LIMMITED):
                    return "LIMMITED";  // 原枚举/上报拼写即为 LIMMITED，保持兼容不改
                default:
                    return "UNKNOWN";
            }
        }

        // ==================== error_code 边沿上报过滤 ====================
        // 消抖拍数：新错误值需连续出现这么多个周期才认定为变化
        static const int kStableCycles = 2;

        bool ErrorCodeEdgeFilter::changed(const std::string &code) {
            if (!m_hasLast) {          // 首周期：携带当前值一次
                m_last = code;
                m_hasLast = true;
                return true;
            }
            if (code == m_last) {      // 状态未变化：省略字段，不重复上报
                m_candidate.clear();
                m_candidateCount = 0;
                return false;
            }
            if (code == "NONE") {      // 恢复（错误消失）：不消抖，立即上报
                m_last = code;
                m_candidate.clear();
                m_candidateCount = 0;
                return true;
            }
            // 新错误值：连续 kStableCycles 拍相同才认定（防枚举边界闪烁）
            if (code == m_candidate) {
                ++m_candidateCount;
            } else {
                m_candidate = code;
                m_candidateCount = 1;
            }
            if (m_candidateCount >= kStableCycles) {
                m_last = code;
                m_candidate.clear();
                m_candidateCount = 0;
                return true;           // 出现/变化：携带真实值
            }
            return false;              // 候选未稳定：等待下一拍
        }

    }  // namespace vwise
}  // namespace cmsr
