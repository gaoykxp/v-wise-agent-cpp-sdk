/**********************************************************************************************************************
    > File Name: cellular_diagnosis.h
    > Desc: 蜂窝诊断判定统一单元——wireless.error_code 枚举推导与注册/驻网状态字符串映射。
    >       驱动路径（DiagSnapshot 快照）与回退路径（boards 层 QENG 查询）共用同一套
    >       枚举语义（6 值）：NONE / SIM_FAULT / REGISTER_FAIL / NOT_REGISTERED /
    >       NO_CELL / WEAK_SIGNAL，优先级同此排列顺序（高→低）。
    >       故障通知已收敛为本单元的 error_code 边沿上报（fault/up 机制退役）。
    >       纯函数、无状态（判定输入全部由调用方给出），供 probe_manager 周期采集调用；
    >       唯一的例外是 ErrorCodeEdgeFilter——持有上一周期值做边沿/消抖。
**********************************************************************************************************************/

#ifndef CELLULAR_DIAGNOSIS_H
#define CELLULAR_DIAGNOSIS_H

#include <string>
#include "modem_hal.h"
#include "rpi_module_interface.h"

namespace cmsr {
    namespace vwise {

        // ==================== wireless.error_code（6 枚举值） ====================
        // 驱动路径：按 DiagSnapshot 真实状态推导，供平台异常监测展示。
        // 优先级：SIM_FAULT > REGISTER_FAIL > NOT_REGISTERED > NO_CELL > WEAK_SIGNAL > NONE
        std::string deriveWirelessErrorCode(const rpi::SimInfo &simInfo,
                                            const ::vwise::modem::DiagSnapshot &snap,
                                            int lteThr, int nrThr);

        // 回退路径：boards 层 QENG 原始查询结果推导（无方言层驱动时保持可用）
        std::string deriveWirelessErrorCodeFallback(
            const rpi::RPIModuleInterface::NetworkWirelessInfo &wirelessInfo,
            bool simFault, int lteThr, int nrThr);

        // ==================== 状态字符串映射（reg_stat / ue_state） ====================
        // QENG <state> → 字符串（驻网/注册/业务态，定界关键判据：
        // LIMSRV=已驻留小区但未注册——指向 SIM/签约/网络侧而非模组硬件）
        const char *ueStateName(::vwise::modem::ServingCell::UeState s);

        // 注册状态字符串（3GPP TS 27.007 stat 语义 + PDP 事实纠偏后的综合判定）
        std::string regStatString(const ::vwise::modem::DiagSnapshot &snap);

        // 回退路径注册状态字符串（boards 层 reg_stat 原始值 → 字符串）
        std::string regStatStringFallback(int regStatRaw);

        // ==================== error_code 边沿上报过滤 ====================
        // 错误状态未变化期间省略 error_code 字段（不重复上报），仅在
        // 出现 / 变化 / 消失的周期携带真实值。状态在内存：进程重启后
        // 首个周期会重新携带一次当前值（重启本身即新事件）。
        // 消抖：新错误值需连续 kStableCycles 拍相同才认定（防枚举边界闪烁，
        // 如 NOT_REGISTERED↔NONE 在快慢刷新交替/QENG 失败保留旧值时抖动）；
        // 恢复（→NONE）不消抖立即上报。
        class ErrorCodeEdgeFilter {
        public:
            // code 与上一周期不同且已稳定 → 记录并返回 true（调用方写入字段）；
            // 相同或未稳定 → 返回 false（调用方省略字段）
            bool changed(const std::string &code);

        private:
            std::string m_last;      // 上一周期已认定的值
            bool m_hasLast = false;  // 是否已有历史（首周期视为有变化）
            std::string m_candidate;      // 待稳定确认的新值
            int m_candidateCount = 0;     // 候选值已连续出现的拍数
        };

    }  // namespace vwise
}  // namespace cmsr

#endif  // CELLULAR_DIAGNOSIS_H
