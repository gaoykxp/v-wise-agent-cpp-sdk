/**********************************************************************************************************************
    > File Name: modem_hal.h
    > Description: 蜂窝模组硬件抽象层（模组无关）
    >
    > 设计依据：《SDK多模组兼容设计-编译选项方案.md》
    > 三条铁律：
    >   1. 接口按"能力"命名，不按 AT 命令命名（getRegistration 而非 getCereg）
    >   2. 数据结构与枚举值统一为 3GPP 标准语义，方言层负责私有值换算
    >   3. 行为差异用 ModemProfile 数据描述，禁止业务层/语义层 #ifdef
    >
    > 业务层（probe_mgr 等）只允许通过 modemDriver() 访问模组能力，
    > 出现任何 AT 命令字符串即违规（见设计文档"设计红线"）。
**********************************************************************************************************************/
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vwise::modem {

// ==================== AT 通道抽象（方言层唯一依赖的传输接口） ====================

// 命令执行结果。error_text 为 "+CME ERROR: <verbose>" 文本（需模组已使能 AT+CMEE=2）
struct AtResult {
    bool ok{false};
    std::vector<std::string> lines;
    std::string error_text;
};

using AtUrcCallback = std::function<void(const std::string& line)>;

class IAtChannel {
public:
    virtual ~IAtChannel() = default;
    // 执行一条 AT 命令并等待最终结果码（OK/ERROR/+CME ERROR/+CMS ERROR）
    virtual AtResult command(const std::string& cmd, int timeout_ms) = 0;
    // 注册 URC 前缀处理器（同前缀重复注册覆盖）。回调运行于 URC 工作线程
    virtual void addUrcHandler(const std::string& prefix, AtUrcCallback cb) = 0;
};

// ==================== 诊断语义事件（方言层把私有 URC 翻译成这些枚举） ====================

enum class DiagEvent {
    ModuleBoot,        // 模组上电/功能恢复完成（RDY / +CFUN: 1）
    ModulePowerDown,   // 模组关机（POWERED DOWN）
    SimRemoved,        // SIM 拔出 / NOT READY（+QSIMSTAT: 1,0 / +CPIN: NOT READY）
    SimInserted,       // SIM 插入 / READY
    NetDenied,         // 注册被拒（CEREG/C5GREG stat=3，URC 携带原因）
    NetNwDeact,        // 网络侧强制去激活承载（+CGEV: NW DEACT）→ 运营商网络侧
    NetNwDetach,       // 网络侧强制去附着（+CGEV: NW DETACH）→ 运营商网络侧
    NetMeDeact,        // 模组侧主动去激活承载（+CGEV: ME DEACT）→ 模组侧
    NetMeDetach,       // 模组侧主动去附着（+CGEV: ME DETACH）→ 模组侧
    PdnAct,            // PDN 连接建立（+CGEV: PDN ACT）
    PdnDeact,          // PDN 连接断开（+CGEV: PDN DEACT）
    RmNetDown,         // 主机侧数据链路断开（+QNETDEVSTATUS state=0）→ 主机/模组接口
    RatChange,         // 制式变化（+QIND: "act"）
};

// 事件责任域标注（供平台侧时间线直接归类）
// module=模组侧 sim=SIM/签约侧 network=运营商网络侧 host=主机/车机侧
const char* diagEventDomain(DiagEvent ev);
const char* diagEventName(DiagEvent ev);

// ==================== 统一数据结构（全部 3GPP 标准语义） ====================

// 注册状态。stat 取值同 3GPP TS 27.007：
// 0=未注册(未搜索) 1=已注册本网 2=搜索中 3=注册被拒 4=未知 5=已注册漫游 8=仅紧急注册
// 不支持的域恒为 -1（语义层按"证据不足"降级，不误判）
struct RegInfo {
    int ps_stat{-1};        // EPS 域（AT+CEREG?）
    int cs_stat{-1};        // CS 域（AT+CREG?）
    int stat_5g{-1};        // 5GS 域（AT+C5GREG? / AT+Q5GREG?）
    int reject_cause{0};    // 注册被拒原因码（CEREG 第 6 段 <reject_cause>）
    std::string tac;        // 跟踪区码（hex）
    std::string ci;         // 小区标识（hex）

    // UE 是否已注册（任一数据域已注册即视为在网）
    bool ueRegistered() const {
        return ps_stat == 1 || ps_stat == 5 || stat_5g == 1 || stat_5g == 5;
    }
};

// 信号信息。模组不支持的字段恒为 -999，语义层自行降级
struct SignalInfo {
    std::string rat;        // "LTE" / "NR5G" / "WCDMA" / "GSM"
    int rsrp{-999};         // dBm
    int rsrq{-999};         // dB
    int sinr{-999};         // dB
    int rssi{-999};         // dBm
    bool valid{false};
};

// 服务小区（含驻网-注册分界判据 ue_state）
struct ServingCell {
    enum class UeState { SEARCH, LIMSRV, NOCONN, CONNECT, UNKNOWN };
    UeState ue_state{UeState::UNKNOWN};   // QENG <state>：驻网/注册/业务态
    std::string mcc, mnc;
    std::string cell_id;    // hex（NR）/ 十进制（LTE）
    std::string pci, tac, band;
    bool valid{false};
};

// 数据承载（PDP/PDU 会话 + 主机侧数据链路）
struct DataBearer {
    bool pdp_active{false}; // 持有有效 IP（AT+CGPADDR）
    bool rmnet_up{false};   // 主机侧 RmNet 数据链路在通（不支持的模组恒 false，不参与判定）
    std::string ip;
};

// ==================== 模组画像：能力与怪癖的数据化描述 ====================

struct ModemProfile {
    const char* vendor{""};     // "quectel" / "simcom" / "fibocom"
    const char* model{""};      // "RG520N"（上报至 system.module，平台按型号分群统计）
    // ---- 能力开关（方言层与语义层据此选择指令与降级路径）----
    bool has_5g{false};
    bool has_c5greg{false};             // 标准 5GS 注册查询（3GPP TS 27.007）
    bool has_q5greg{false};             // Quectel 私有 5G 注册查询
    bool has_ceer{false};               // 扩展错误报告（AT+CEER，失败原因文本）
    bool has_adc{false};                // 供电电压采集（AT+QADC）
    // ---- 怪癖标志（集中在此声明，禁止散落 #ifdef）----
    bool quirk_cereg_zero_on_sa{false};     // SA 下 EPS CEREG 恒 0，注册判定须看 5GS 域
    bool quirk_pdp_implies_registered{false}; // PDP 持有效 IP ⇒ 实际已注册（注册查询失真兜底）
};

// ==================== 诊断快照（一个采集周期的完整定界证据） ====================

struct DiagSnapshot {
    RegInfo reg;                 // 原始注册查询值（证据保留，不做覆盖）
    SignalInfo signal;
    ServingCell cell;
    DataBearer bearer;
    std::string ceer;            // 注册被拒时补查的 AT+CEER 失败原因文本（可为空）
    bool effective_registered{false};  // 纠偏后注册判定（5GS 域 + PDP 事实）
};

// ==================== 模组驱动接口 ====================

class IModemDriver {
public:
    virtual ~IModemDriver() = default;

    virtual const ModemProfile& profile() const = 0;

    // 绑定 AT 通道并完成该模组的 URC 使能与处理器注册（幂等，可重复调用）
    virtual bool init(IAtChannel& ch) = 0;

    // 一次采集全部无线指标（注册/信号/服务小区/承载），含怪癖纠偏
    virtual bool getDiagSnapshot(DiagSnapshot& out) = 0;

    // 仅采集信号与服务小区（单条 AT+QENG="servingcell"，不发注册/承载查询）：
    // 供高频周期采集用——注册三域（C5GREG/CEREG/CREG）与 PDP（CGPADDR）为慢变量，
    // 由语义层节流走 getDiagSnapshot 刷新，周期内只跑本快路径
    virtual bool getServingSignal(SignalInfo& sig, ServingCell& cell) = 0;

    // 扩展错误报告（最近一次失败操作的释放原因，手册 §3.2/§12.9）。不支持返回 false
    virtual bool getExtendedError(std::string& text) = 0;
};

// 编译期选定驱动的唯一入口（绑定见 modem_registry.cpp，CMake 选项 VWISE_MODEM）
IModemDriver& modemDriver();

} // namespace vwise::modem
