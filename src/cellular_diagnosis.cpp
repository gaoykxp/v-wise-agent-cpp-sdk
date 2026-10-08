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
        // 枚举（9 值）：NONE / SIM_FAULT / REGISTER_FAIL / NET_RESOURCE_SHORT /
        //              NOT_REGISTERED / NO_CELL / WEAK_SIGNAL / REBOOT / G4G5_SWITCH
        // 前 7 值为状态推导值（本函数），优先级：
        //   SIM_FAULT > REGISTER_FAIL > NET_RESOURCE_SHORT > NOT_REGISTERED
        //   > NO_CELL > WEAK_SIGNAL > NONE
        // （SIM/显式被拒最特异；NET_RESOURCE_SHORT 是被拒中的资源性细分；
        //   未注册是"无数据业务"的定论；无小区常是未注册的前置/伴随；
        //   弱信号时业务可能仍可用，排最后）
        // REBOOT / G4G5_SWITCH 为事件型值（kErrorCodeReboot / kErrorCodeRatSwitch，
        // 见 cellular_diagnosis.h）：前者启动后首条 Full 消息携带一次，后者由
        // RatSwitchDetector 检测到 4G↔5G 跳变的周期携带一次，均不参与本优先级链。
        std::string deriveWirelessErrorCode(const rpi::SimInfo &simInfo,
                                            const ::vwise::modem::DiagSnapshot &snap,
                                            int lteThr, int nrThr) {
            if (simInfo.status == rpi::SimStatus::ABSENT ||
                simInfo.status == rpi::SimStatus::LOCKED) {
                return "SIM_FAULT";
            }
            if (snap.reg.ps_stat == 3 || snap.reg.cs_stat == 3 || snap.reg.stat_5g == 3) {
                // 资源性拒绝细分：被拒域的原因码 22（congestion）→
                // NET_RESOURCE_SHORT。22 在 EMM（TS 24.301，CEREG 携带）与
                // 5GMM（TS 24.501，C5GREG/Q5GREG 携带）两码空间同码同义，无需
                // 区分域。原因码仅对应域被拒时由 parseRegLine 解析（stat==3 且
                // 响应含第 7 段），CS 域被拒无原因码，维持 REGISTER_FAIL。
                // SA 网参考 quirk_cereg_zero_on_sa：EPS 域常恒 0，资源性拒绝
                // 事实在 5GS 域（stat_5g==3 + reject_cause_5g）
                if ((snap.reg.ps_stat == 3 &&
                     snap.reg.reject_cause == kRejectCauseCongestion) ||
                    (snap.reg.stat_5g == 3 &&
                     snap.reg.reject_cause_5g == kRejectCauseCongestion)) {
                    return "NET_RESOURCE_SHORT";
                }
                return "REGISTER_FAIL";
            }
            if (!snap.effective_registered) {
                return "REGISTER_FAIL";
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
        // 按 LTE 阈值分档，与原内联逻辑一致。boards 层不采集拒绝原因码，
        // 被拒时无法细分 NET_RESOURCE_SHORT，一律 REGISTER_FAIL
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

        // ==================== G4G5_SWITCH（4/5G 制式切换边沿检测） ====================
        // RAT 值域（两路径已归一）：驱动路径 QENG → "NR5G"（SA/NSA/EN-DC 均归一，
        // NR 行优先于 LTE 行）/ "LTE"（含 CESQ 兜底）；回退路径 technology →
        // "NR5G" / "LTE" / "Unknown"。非 4/5G 制式（WCDMA/GSM/空/Unknown）不在
        // 切换判定域内。
        RatSwitchDetector::Fam RatSwitchDetector::family(const std::string &rat) {
            if (rat.rfind("NR5G", 0) == 0) return Fam::Nr;   // NR5G / NR5G-SA / NR5G-NSA
            if (rat == "LTE") return Fam::Lte;
            return Fam::Unknown;
        }

        bool RatSwitchDetector::switched(const std::string &rat) {
            const Fam fam = family(rat);
            if (fam == Fam::Unknown) return false;   // 无效 RAT：不判定、不动基线
            if (m_last == Fam::Unknown) {            // 首个有效 RAT：只建基线
                m_last = fam;
                return false;
            }
            const bool hit = (fam != m_last);        // LTE↔NR5G 跳变即切换事件
            m_last = fam;
            return hit;
        }

    }  // namespace vwise
}  // namespace cmsr
