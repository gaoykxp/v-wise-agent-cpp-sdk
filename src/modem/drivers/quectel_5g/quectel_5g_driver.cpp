#include "quectel_5g_driver.h"

#include <cctype>
#include <cstdlib>

#include "diag_event_bus.h"
#include "log.h"

namespace vwise::modem {

// ==================== 方言层内部工具 ====================

namespace {

std::string trim(std::string s) {
    // 去引号后 trim
    size_t b = 0, e = s.size();
    while (b < e && (std::isspace((unsigned char)s[b]) || s[b] == '"')) b++;
    while (e > b && (std::isspace((unsigned char)s[e - 1]) || s[e - 1] == '"')) e--;
    return s.substr(b, e - b);
}

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(trim(cur)); cur.clear(); }
        else cur += c;
    }
    out.push_back(trim(cur));
    return out;
}

bool parseInt(const std::string& s, int& val) {
    if (s.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    val = static_cast<int>(v);
    return true;
}

bool startsWith(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

// 解析注册类查询响应行（CEREG/CREG/C5GREG/Q5GREG 同构）：
//   查询响应: +XXX: <n>,<stat>[,<tac>,<ci>[,<AcT>[,<cause_type>,<reject_cause>]]]
// 取 parts[1] 为 stat；且仅当 parts[1] 可解析为整数时采信——
// 该启发式同时过滤掉"查询进行中插入的同前缀 URC"（URC 无 <n> 段，parts[1] 为带引号的
// tac 字符串，解析必然失败），避免 URC 串扰导致 stat 误读为 0。
// 注：RG520N 手册基础格式仅到 <AcT>（parts[4]）；部分固件在 stat=3 时按 TS 27.007
// 扩展追加 <cause_type>,<reject_cause>（parts[5]/[6]），无该段时 cause 保持 0。
// 返回 true 表示成功取到 stat。
bool parseRegLine(const std::string& line, const char* prefix,
                  int& stat, int* reject_cause = nullptr) {
    if (!startsWith(line, prefix)) return false;
    auto parts = splitCsv(line.substr(std::string(prefix).size()));
    if (parts.size() < 2) return false;
    if (!parseInt(parts[1], stat)) return false;
    if (reject_cause && stat == 3 && parts.size() >= 7) {
        int cause = 0;
        if (parseInt(parts[6], cause)) *reject_cause = cause;
    }
    return true;
}

ServingCell::UeState parseUeState(const std::string& s) {
    if (s == "SEARCH")  return ServingCell::UeState::SEARCH;
    if (s == "LIMSRV")  return ServingCell::UeState::LIMSRV;
    if (s == "NOCONN")  return ServingCell::UeState::NOCONN;
    if (s == "CONNECT") return ServingCell::UeState::CONNECT;
    return ServingCell::UeState::UNKNOWN;
}

} // namespace

// ==================== Profile（RG520N/RM520N 同族画像） ====================

const ModemProfile& Quectel5GDriver::profile() const {
    static const ModemProfile p{
        "quectel",                    // vendor
        "RG520N",                     // model（驱动画像标签，同族 RM520N 共用；上报取 AT+CGMM 实测值）
        /*has_5g=*/true,
        /*has_c5greg=*/true,          // 标准 5GS 注册查询（手册 §5.5）
        /*has_q5greg=*/true,          // Quectel 私有 5G 注册（老固件兜底）
        /*has_ceer=*/true,            // 扩展错误报告（手册 §3.2）
        /*has_adc=*/true,             // 供电电压（手册 §11.3）
        // SA 下 EPS CEREG 恒 0（注册实际在 5GS 域），注册判定必须看 5GS 域
        /*quirk_cereg_zero_on_sa=*/true,
        // PDP 持有效 IP ⇒ 数据承载在通、实际已注册（实测 SA 固件注册查询失真）
        /*quirk_pdp_implies_registered=*/true
    };
    return p;
}

// ==================== init：URC 使能 + 处理器注册 ====================

bool Quectel5GDriver::init(IAtChannel& ch) {
    ch_ = &ch;

    // ---- 使能（与 RPIModuleInterface::init 启动序列幂等，重复下发无害）----
    ch_->command("AT+CMEE=2", 1500);          // 错误报告 verbose：裸 ERROR → +CME ERROR: <text>（§2.23）
    // CEREG/C5GREG n=3：URC 带位置 + EMM 拒绝原因（stat=3 时尾随 <cause_type>,<reject_cause>，
    // §5.5）。老固件不支持 n=3 时降级 n=2（仅位置）
    if (!ch_->command("AT+CEREG=3", 1500).ok)
        ch_->command("AT+CEREG=2", 1500);
    if (!ch_->command("AT+C5GREG=3", 1500).ok)
        ch_->command("AT+C5GREG=2", 1500);
    ch_->command("AT+CGEREP=1", 1500);        // PDP 事件 URC（§9.7，与启动序列同值）
    ch_->command("AT+QNETDEVSTATUS=1", 1500); // 主机侧 RmNet 链路状态 URC（§9.11）

    registerUrcHandlers();
    LogInfo << "[Quectel5G] driver initialized";
    return true;
}

void Quectel5GDriver::registerUrcHandlers() {
    // 注册即覆盖 RPIModuleInterface 的纯日志 handler（UrcDispatcher 同前缀覆盖语义），
    // 驱动内负责翻译为语义事件并保留日志。
    ch_->addUrcHandler("+CGEV:", [this](const std::string& l) { onCgev(l); });
    ch_->addUrcHandler("+QSIMSTAT:", [this](const std::string& l) { onQsimstat(l); });
    ch_->addUrcHandler("+CPIN:", [this](const std::string& l) { onCpin(l); });
    ch_->addUrcHandler("+C5GREG:", [this](const std::string& l) { onRegUrc(l, "network"); });
    ch_->addUrcHandler("+CEREG:", [this](const std::string& l) { onRegUrc(l, "network"); });
    ch_->addUrcHandler("+Q5GREG:", [this](const std::string& l) { onRegUrc(l, "network"); });
    ch_->addUrcHandler("RDY", [this](const std::string& l) { onReady(l); });
    ch_->addUrcHandler("POWERED DOWN", [this](const std::string& l) { onPoweredDown(l); });
    ch_->addUrcHandler("+CFUN:", [this](const std::string& l) { onCfun(l); });
    ch_->addUrcHandler("+QIND:", [this](const std::string& l) { onQind(l); });
    ch_->addUrcHandler("+QNETDEVSTATUS:", [this](const std::string& l) { onQnetdevstatus(l); });
}

// ==================== 查询子项 ====================

void Quectel5GDriver::queryRegistration(RegInfo& out) {
    // 5GS 域：标准 C5GREG 优先，Quectel 私有 Q5GREG 兜底（SA 下 EPS CEREG 恒 0，
    // 5GS 域才是注册事实来源——quirk_cereg_zero_on_sa）
    if (profile().has_c5greg) {
        auto r = ch_->command("AT+C5GREG?", 1500);
        for (const auto& l : r.lines)
            if (parseRegLine(l, "+C5GREG:", out.stat_5g)) break;
    }
    if (out.stat_5g < 0 && profile().has_q5greg) {
        auto r = ch_->command("AT+Q5GREG?", 1500);
        for (const auto& l : r.lines)
            if (parseRegLine(l, "+Q5GREG:", out.stat_5g)) break;
    }

    // EPS 域（含拒绝原因码）
    {
        auto r = ch_->command("AT+CEREG?", 1500);
        for (const auto& l : r.lines)
            if (parseRegLine(l, "+CEREG:", out.ps_stat, &out.reject_cause)) break;
    }
    // CS 域（短信通道判定）
    {
        auto r = ch_->command("AT+CREG?", 1500);
        for (const auto& l : r.lines)
            if (parseRegLine(l, "+CREG:", out.cs_stat)) break;
    }
}

void Quectel5GDriver::queryServing(SignalInfo& sig, ServingCell& cell) {
    // 一次 AT+QENG="servingcell" 同时取 UE 状态/小区/信号（§5.20）
    // 响应形态（手册）：
    //   SA:     +QENG: "servingcell",<state>,"NR5G-SA",<duplex>,<MCC>,<MNC>,<cellID>,<PCID>,<TAC>,<ARFCN>,<band>,<bw>,<RSRP>,<RSRQ>,<SINR>,<srxlev>,<scs>
    //   LTE:    +QENG: "servingcell",<state>,"LTE",[<is_tdd>,]<MCC>,<MNC>,<cellID>,<PCID>,<earfcn>,<band>,...（实测有无 is_tdd 列随固件，做偏移自适应）
    //   EN-DC:  +QENG: "servingcell",<state>          ← 仅状态行
    //           +QENG: "LTE",<is_tdd>,<MCC>,...
    //           +QENG: "NR5G-NSA",<MCC>,<MNC>,<PCID>,<RSRP>,<SINR>,<RSRQ>,<ARFCN>,<band>,<bw>,<scs>
    auto r = ch_->command("AT+QENG=\"servingcell\"", 2000);
    bool nrTaken = false, lteTaken = false;
    ServingCell cellNr, cellLte;
    SignalInfo sigNr, sigLte;

    for (const auto& l : r.lines) {
        if (!startsWith(l, "+QENG:")) continue;
        auto parts = splitCsv(l.substr(6));
        if (parts.size() < 2) continue;
        const std::string& head = parts[0];

        // ---- UE 状态：<state> 恒在 servingcell 行 parts[1]（SEARCH/LIMSRV/NOCONN/CONNECT）----
        if (head == "servingcell" && parts.size() >= 2) {
            cell.ue_state = parseUeState(parts[1]);
        }

        if (head == "servingcell" && parts.size() >= 15 && !nrTaken && parts[2].find("NR5G") != std::string::npos) {
            // ---- NR5G-SA 整行 ----
            // parts: 3=duplex 4=MCC 5=MNC 6=cellID 7=PCID 8=TAC 9=ARFCN 10=band 11=bw 12=RSRP 13=RSRQ 14=SINR
            cellNr.mcc = parts[4]; cellNr.mnc = parts[5];
            cellNr.cell_id = parts[6]; cellNr.pci = parts[7]; cellNr.tac = parts[8]; cellNr.band = parts[10];
            cellNr.valid = true;
            int rsrp = 0, rsrq = 0, sinr = 0;
            if (parseInt(parts[12], rsrp) && rsrp < 0) {
                sigNr.rat = "NR5G"; sigNr.rsrp = rsrp;
                if (parseInt(parts[13], rsrq) && rsrq < 0) sigNr.rsrq = rsrq;
                if (parseInt(parts[14], sinr)) sigNr.sinr = sinr;
                sigNr.valid = true;
            }
            nrTaken = true;
        } else if (head == "NR5G-NSA" && !nrTaken && parts.size() >= 9) {
            // ---- EN-DC 的 NR 辅小区行（手册 §5.20，无 state）----
            // parts: 1=MCC 2=MNC 3=PCID 4=RSRP 5=SINR 6=RSRQ 7=ARFCN 8=band
            cellNr.mcc = parts[1]; cellNr.mnc = parts[2]; cellNr.pci = parts[3]; cellNr.band = parts[8];
            cellNr.valid = true;
            int rsrp = 0;
            if (parseInt(parts[4], rsrp) && rsrp < 0) {
                sigNr.rat = "NR5G"; sigNr.rsrp = rsrp;
                int v = 0;
                if (parseInt(parts[5], v)) sigNr.sinr = v;
                if (parseInt(parts[6], v) && v < 0) sigNr.rsrq = v;
                sigNr.valid = true;
            }
            nrTaken = true;
        } else if (head == "servingcell" && parts.size() >= 10 && parts[2] == "LTE" && !lteTaken) {
            // ---- LTE 整行（is_tdd 列存在性自适应：parts[3] 为数字则无 is_tdd）----
            int off = 0;
            int probe = 0;
            if (!parseInt(parts[3], probe)) off = 1;   // parts[3]="TDD"/"FDD" → 有 is_tdd 列
            // 列序（含 is_tdd 时 off=1）：3/4=MCC,MNC 5/6=cellID,PCID 7=EARFCN 8=band
            //                     9/10=UL,DL bw 11=TAC 12=RSRP 13=RSRQ 14=RSSI 15=SINR
            const int mcc_i = 3 + off, mnc_i = 4 + off, cid_i = 5 + off, pci_i = 6 + off,
                      band_i = 8 + off, tac_i = 11 + off, rsrp_i = 12 + off,
                      rsrq_i = 13 + off, sinr_i = 15 + off;
            if (parts.size() > rsrp_i) {
                cellLte.mcc = parts[mcc_i]; cellLte.mnc = parts[mnc_i];
                cellLte.cell_id = parts[cid_i]; cellLte.pci = parts[pci_i];
                cellLte.band = parts[band_i];
                cellLte.tac = parts[tac_i];
                cellLte.valid = true;
                int rsrp = 0;
                if (parseInt(parts[rsrp_i], rsrp) && rsrp < 0) {
                    sigLte.rat = "LTE"; sigLte.rsrp = rsrp;
                    int v = 0;
                    if (parseInt(parts[rsrq_i], v) && v < 0) sigLte.rsrq = v;
                    if (parseInt(parts[sinr_i], v)) sigLte.sinr = v;
                    sigLte.valid = true;
                }
                lteTaken = true;
            }
        } else if (head == "LTE" && parts.size() >= 15 && !lteTaken) {
            // ---- EN-DC 的 LTE 锚点行（手册 §5.20，无 state）----
            // parts: 1=is_tdd 2=MCC 3=MNC 4=cellID 5=PCID 6=earfcn 7=band
            //        8/9=UL,DL bw 10=TAC 11=RSRP 12=RSRQ 13=RSSI 14=SINR
            cellLte.mcc = parts[2]; cellLte.mnc = parts[3];
            cellLte.cell_id = parts[4]; cellLte.pci = parts[5]; cellLte.band = parts[7];
            cellLte.tac = parts[10];
            cellLte.valid = true;
            int rsrp = 0;
            if (parseInt(parts[11], rsrp) && rsrp < 0) {
                sigLte.rat = "LTE"; sigLte.rsrp = rsrp;
                int v = 0;
                if (parseInt(parts[12], v) && v < 0) sigLte.rsrq = v;
                if (parseInt(parts[14], v)) sigLte.sinr = v;
                sigLte.valid = true;
            }
            lteTaken = true;
        }
    }

    // NR 优先（SA/NSA），LTE 回退；均无则 CESQ 兜底取 LTE 信号
    if (nrTaken) {
        sig = sigNr;
        cellNr.ue_state = cell.ue_state;
        cell = cellNr;
    } else if (lteTaken) {
        sig = sigLte;
        cellLte.ue_state = cell.ue_state;
        cell = cellLte;
    } else {
        queryLteSignalByCesq(sig);
    }
}

void Quectel5GDriver::queryLteSignalByCesq(SignalInfo& sig) {
    // +CESQ: <rxlev>,<ber>,<rscp>,<ecn0>,<rsrq>,<rsrp>（编码值需换算，§5.9 同族）
    auto r = ch_->command("AT+CESQ", 2000);
    for (const auto& l : r.lines) {
        if (!startsWith(l, "+CESQ:")) continue;
        auto parts = splitCsv(l.substr(6));
        if (parts.size() < 6) continue;
        int rsrpCode = 0, rsrqCode = 0, rxlev = 0;
        if (!parseInt(parts[5], rsrpCode) || !parseInt(parts[4], rsrqCode)) continue;
        if (rsrpCode == 255 || rsrpCode > 97) continue;   // 255=未知
        sig.rat = "LTE";
        sig.rsrp = -140 + rsrpCode;                       // 0=-140dBm ... 97=-44dBm
        if (rsrqCode != 255 && rsrqCode <= 34)
            sig.rsrq = static_cast<int>(-19.5 + rsrqCode * 0.5);
        if (parseInt(parts[0], rxlev) && rxlev != 99 && rxlev <= 63)
            sig.rssi = -110 + rxlev;
        sig.valid = true;
        return;
    }
}

void Quectel5GDriver::queryBearer(DataBearer& out) {
    // AT+CGPADDR=1：数据承载可能不在 CID 1（多上下文/SA PDU 会话），
    // 查到空/0.0.0.0 时改查全部上下文取第一个有效地址
    std::string ip;
    auto r = ch_->command("AT+CGPADDR=1", 1500);
    for (const auto& l : r.lines) {
        if (!startsWith(l, "+CGPADDR:")) continue;
        auto parts = splitCsv(l.substr(9));
        if (parts.size() >= 2 && !parts[1].empty()) { ip = parts[1]; break; }
    }
    if (ip.empty() || ip == "0.0.0.0") {
        auto r2 = ch_->command("AT+CGPADDR", 1500);
        for (const auto& l : r2.lines) {
            if (!startsWith(l, "+CGPADDR:")) continue;
            auto parts = splitCsv(l.substr(9));
            if (parts.size() >= 2 && !parts[1].empty() && parts[1] != "0.0.0.0") { ip = parts[1]; break; }
        }
    }
    if (!ip.empty()) {
        out.ip = ip;
        out.pdp_active = (ip != "0.0.0.0");
    }
    out.rmnet_up = out.pdp_active;   // QNETDEVSTATUS URC 覆盖；无事件时以 PDP 近似
}

// ==================== 诊断快照（一个采集周期的完整证据 + 纠偏） ====================

bool Quectel5GDriver::getDiagSnapshot(DiagSnapshot& out) {
    if (!ch_) return false;
    out = DiagSnapshot{};   // 字段缺省值即"证据不足"语义

    queryRegistration(out.reg);
    queryServing(out.signal, out.cell);
    queryBearer(out.bearer);

    // 注册被拒时补查 AT+CEER（最近一次失败操作的释放原因文本，§3.2/原因表 §12.9）。
    // 仅本函数（30s 慢变批次）执行，不占 3s 周期；CEER 只保留最近一次失败原因，
    // 被拒为瞬时态时文本可能已被后续操作覆盖——仅作参考证据，与 reg 拒绝码互不覆盖。
    if (profile().has_ceer &&
        (out.reg.ps_stat == 3 || out.reg.cs_stat == 3 || out.reg.stat_5g == 3)) {
        std::string text;
        if (getExtendedError(text)) out.ceer = text;
    }

    // ---- 怪癖纠偏（集中此处，profile 标志驱动）----
    // 1) SA 下 CEREG 恒 0：注册事实看 5GS 域（ueRegistered 已综合）
    // 2) PDP 持有效 IP ⇒ 数据承载在通、实际已注册（注册查询失真兜底）
    out.effective_registered = out.reg.ueRegistered();
    if (!out.effective_registered && profile().quirk_pdp_implies_registered && out.bearer.pdp_active) {
        out.effective_registered = true;
    }
    return true;
}

bool Quectel5GDriver::getServingSignal(SignalInfo& sig, ServingCell& cell) {
    // 快路径：仅 AT+QENG="servingcell"（信号/小区/ue_state），高频周期采集用
    if (!ch_) return false;
    queryServing(sig, cell);
    return true;
}

bool Quectel5GDriver::getExtendedError(std::string& text) {
    // AT+CEER → +CEER: <text>：最近一次失败操作的释放原因（§3.2，原因表 §12.9）
    auto r = ch_->command("AT+CEER", 1500);
    for (const auto& l : r.lines) {
        if (!startsWith(l, "+CEER:")) continue;
        text = trim(l.substr(6));
        return true;
    }
    return false;
}

// ==================== URC 处理器：私有 URC → 语义事件 ====================

void Quectel5GDriver::emit(DiagEvent ev, const std::string& raw) {
    DiagEventBus::getInstance().publish(ev, raw);
}

void Quectel5GDriver::onCgev(const std::string& line) {
    // §9.7：NW*=网络侧强制 / ME*=模组侧主动 —— "网络侧 vs 模组侧"最直接指证
    if (line.find("NW DEACT") != std::string::npos)      emit(DiagEvent::NetNwDeact, line);
    else if (line.find("NW DETACH") != std::string::npos) emit(DiagEvent::NetNwDetach, line);
    else if (line.find("ME DEACT") != std::string::npos)  emit(DiagEvent::NetMeDeact, line);
    else if (line.find("ME DETACH") != std::string::npos) emit(DiagEvent::NetMeDetach, line);
    else if (line.find("PDN ACT") != std::string::npos)   emit(DiagEvent::PdnAct, line);
    else if (line.find("PDN DEACT") != std::string::npos) emit(DiagEvent::PdnDeact, line);
    else LogInfo << "[Quectel5G] URC CGEV: " << line;
}

void Quectel5GDriver::onQsimstat(const std::string& line) {
    // +QSIMSTAT: <enable>,<inserted_status>：0=拔出 1=插入 2=未知（初始化前）
    auto parts = splitCsv(line.substr(std::string("+QSIMSTAT:").size()));
    int status = -1;
    if (!parts.empty()) parseInt(parts.back(), status);
    if (status == 0)      emit(DiagEvent::SimRemoved, line);
    else if (status == 1) emit(DiagEvent::SimInserted, line);
    else LogInfo << "[Quectel5G] URC QSIMSTAT: " << line;
}

void Quectel5GDriver::onCpin(const std::string& line) {
    if (line.find("NOT READY") != std::string::npos) emit(DiagEvent::SimRemoved, line);
    else if (line.find("READY") != std::string::npos) emit(DiagEvent::SimInserted, line);
    else LogInfo << "[Quectel5G] URC CPIN: " << line;
}

void Quectel5GDriver::onRegUrc(const std::string& line, const char* /*domainHint*/) {
    // +CEREG:/+C5GREG:/+Q5GREG: URC 格式 +XXX: <stat>[,...]（无 <n> 段，parts[0] 即 stat）
    int stat = -1;
    const char* prefix = "+CEREG:";
    if (startsWith(line, "+C5GREG:")) prefix = "+C5GREG:";
    else if (startsWith(line, "+Q5GREG:")) prefix = "+Q5GREG:";
    auto parts = splitCsv(line.substr(std::string(prefix).size()));
    if (!parts.empty()) parseInt(parts[0], stat);
    if (stat == 3) emit(DiagEvent::NetDenied, line);   // 注册被拒；n=3 时 URC 尾随 <cause_type>,<reject_cause>，原始行保留证据
    else LogInfo << "[Quectel5G] URC " << prefix << " " << line;
}

void Quectel5GDriver::onReady(const std::string& line) {
    // RDY：模组初始化完成。运行期出现即"模组发生过重启"——一票否决"网络侧"假设
    emit(DiagEvent::ModuleBoot, line);
}

void Quectel5GDriver::onPoweredDown(const std::string& line) {
    emit(DiagEvent::ModulePowerDown, line);
}

void Quectel5GDriver::onCfun(const std::string& line) {
    // +CFUN: 1 → 全功能恢复（0→1 翻转即软重启完成）
    if (line.find(": 1") != std::string::npos) emit(DiagEvent::ModuleBoot, line);
    else LogInfo << "[Quectel5G] URC CFUN: " << line;
}

void Quectel5GDriver::onQind(const std::string& line) {
    // +QIND: "act",<act> 制式变化；其余（csq/smsfull 等）仅日志
    if (line.find("\"act\"") != std::string::npos) emit(DiagEvent::RatChange, line);
    else LogInfo << "[Quectel5G] URC QIND: " << line;
}

void Quectel5GDriver::onQnetdevstatus(const std::string& line) {
    // +QNETDEVSTATUS: <on>,<state>[,<ip_type>,<profile>]：state 0=RmNet 断开（§9.11）
    // 蜂窝 PDP 正常但 RmNet 断 → 模组-主机数据接口问题（host 域）
    auto parts = splitCsv(line.substr(std::string("+QNETDEVSTATUS:").size()));
    if (parts.size() >= 2 && parts[1] == "0") emit(DiagEvent::RmNetDown, line);
    else LogInfo << "[Quectel5G] URC QNETDEVSTATUS: " << line;
}

} // namespace vwise::modem
