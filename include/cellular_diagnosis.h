/**********************************************************************************************************************
    > File Name: cellular_diagnosis.h
    > Desc: 蜂窝诊断判定统一单元——wireless.error_code 枚举推导与注册/驻网状态字符串映射。
    >       驱动路径（DiagSnapshot 快照）与回退路径（boards 层 QENG 查询）共用同一套
    >       枚举语义（9 值）：NONE / SIM_FAULT / REGISTER_FAIL / NET_RESOURCE_SHORT /
    >       NOT_REGISTERED / NO_CELL / WEAK_SIGNAL / REBOOT / G4G5_SWITCH。
    >       前 7 值为状态推导值（NET_RESOURCE_SHORT 是 REGISTER_FAIL 的资源性
    >       拒绝细分），优先级同此排列顺序（高→低）；REBOOT 与 G4G5_SWITCH
    >       为事件型值（不参与优先级链，见下方常量与检测器）。
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

        // ==================== wireless.error_code（状态推导，7 枚举值） ====================
        // 驱动路径：按 DiagSnapshot 真实状态推导，供平台异常监测展示。
        // 优先级：SIM_FAULT > REGISTER_FAIL > NET_RESOURCE_SHORT > NOT_REGISTERED
        //         > NO_CELL > WEAK_SIGNAL > NONE
        // NET_RESOURCE_SHORT：注册被拒且被拒域原因码为 22（congestion，网络
        // 资源不足）——REGISTER_FAIL 的资源性细分，与 REGISTER_FAIL 同档判定、
        // 仅返回值更特异。仅驱动路径可判（EPS 域原因码由 CEREG? 解析，
        // 5GS 域由 C5GREG?/Q5GREG? 解析——SA 网下 EPS 域恒 0 时 5GS 域是事实
        // 来源；22 在 EMM/5GMM 两码空间同码同义。回退路径 boards 层不采集
        // 原因码，被拒时维持 REGISTER_FAIL）。
        // 判定成立时对应域原因码同帧携带（EPS：wireless.reg_err_code；
        // 5GS：diag.reg_cause_5g），平台可交叉核验。
        inline constexpr int kRejectCauseCongestion = 22;  // 3GPP TS 24.301 EMM / TS 24.501 5GMM cause #22
        std::string deriveWirelessErrorCode(const rpi::SimInfo &simInfo,
                                            const ::vwise::modem::DiagSnapshot &snap,
                                            int lteThr, int nrThr);

        // 回退路径：boards 层 QENG 原始查询结果推导（无方言层驱动时保持可用）。
        // 注：boards 层不采集拒绝原因码（rej_cause 恒 0），资源性拒绝无法细分，
        // 被拒时一律 REGISTER_FAIL
        std::string deriveWirelessErrorCodeFallback(
            const rpi::RPIModuleInterface::NetworkWirelessInfo &wirelessInfo,
            bool simFault, int lteThr, int nrThr);

        // ==================== REBOOT（事件型枚举值，第 7 值） ====================
        // 启动完成事件，不参与上方状态优先级链（无法从快照推导）。判定依据
        // （检测逻辑见 probe_manager，任一命中即随下一条消息的 wireless.error_code
        // 携带 "REBOOT"）：
        //   1) 运行期 URC +QIND: "PB DONE"（模组启动/重启完成；SDK 启动早于
        //      模组启动完成时经 connect() 预置前缀首周期可检出）
        //   2) SDK 进程启动：首条 Full 消息携带 sw_version（编译期常量，无 AT
        //      依赖）即触发——补上 SDK 晚于模组启动完成的盲区，SDK 进程每次
        //      启动各报一次
        // REBOOT 拍跳过边沿过滤（保持无历史状态），下一周期按首周期语义携带
        // 一次真实状态值，此后回归边沿上报。
        inline constexpr const char *kErrorCodeReboot = "REBOOT";

        // ==================== G4G5_SWITCH（事件型枚举值，第 8 值） ====================
        // 4G(LTE) ↔ 5G(NR5G) 网络制式切换事件，由 RatSwitchDetector 逐周期比较
        // 有效 RAT 检测（跳变发生的那个周期随消息携带一次）。同样不参与状态
        // 优先级链。同周期与状态错误冲突时状态错误优先（见 probe_manager）。
        inline constexpr const char *kErrorCodeRatSwitch = "G4G5_SWITCH";

        // 4/5G 制式切换边沿检测器：喂入本周期 RAT（驱动路径 QENG 归一为
        // "NR5G"/"LTE"，回退路径 technology 同值域），与上周期比较。
        // 无效 RAT（查询失败/空/"Unknown"/WCDMA/GSM 等非 4/5G 制式）的周期
        // 不判定也不动基线——QENG 瞬时失败不会造成假切换；首个有效 RAT 只
        // 建基线不触发（开机驻留在哪个制式不是"切换"）。
        class RatSwitchDetector {
        public:
            // 返回 true 表示本周期发生了 4G↔5G 切换（调用方随消息上报一次）
            bool switched(const std::string &rat);

        private:
            enum class Fam { Unknown, Lte, Nr };
            static Fam family(const std::string &rat);   // "NR5G*"→Nr；"LTE"→Lte；其余 Unknown
            Fam m_last = Fam::Unknown;
        };

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
