#include "rpi_at_client.h"

#include <algorithm>
#include <cctype>
#include "log.h"

namespace tbox {

static bool starts_with(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string AtClient::trim(std::string s)
{
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::optional<std::string> AtClient::first_nonempty_line(const std::vector<std::string>& lines)
{
    for (const auto& l : lines) {
        if (!l.empty() && l != "OK" && l != "ERROR")
            return l;
    }
    return std::nullopt;
}

std::vector<std::string> AtClient::split_csv(const std::string& s)
{
    std::vector<std::string> parts;
    std::string cur;
    bool in_quotes = false;

    for (char ch : s) {
        if (ch == '"') {
            in_quotes = !in_quotes;
        } else if (ch == ',' && !in_quotes) {
            parts.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty())
        parts.push_back(trim(cur));

    return parts;
}

std::optional<int> AtClient::parse_int(const std::string& s)
{
    try {
        return std::stoi(s);
    } catch (...) {
        return std::nullopt;
    }
}

AtClient::~AtClient()
{
    if (reading_ || reader_.joinable())
        disconnect();
}

bool AtClient::connect(const std::string& port_name, int baud_rate)
{
    if (!port_.open(port_name, baud_rate))
        return false;

    acc_.reset();
    urc_.start();
    reading_ = true;
    reader_ = std::thread(&AtClient::reader_loop, this);

    // Disable command echo so line classification is unambiguous. Drop any
    // pending partial line before sending.
    command("ATE0", std::chrono::milliseconds(1500));
    return true;
}

void AtClient::disconnect()
{
    // 1) Mark shutdown and wake any blocked command() / serialized waiters so
    //    they return failure immediately. Done BEFORE joining the reader and
    //    the URC worker so a callback stuck in command() does not hang until
    //    its timeout.
    reading_ = false;
    {
        std::lock_guard<std::mutex> lk(cmd_mtx_);
        shutting_down_ = true;
        if (pending_) {
            pending_->done = true;
            pending_->ok = false;
            pending_->cv.notify_all();
        }
        cmd_in_flight_ = false;       // release serialization slot
        cmd_serial_cv_.notify_all();
    }

    // 2) Stop the reader. It stops dispatching new URCs, so the URC worker's
    //    queue will drain and its worker can exit.
    if (reader_.joinable())
        reader_.join();

    // 3) Stop the URC worker (drains remaining queued URCs then joins).
    urc_.stop();

    // 4) Reset state.
    {
        std::lock_guard<std::mutex> lk(cmd_mtx_);
        pending_.reset();
        last_cmd_.clear();
        shutting_down_ = false;
    }

    port_.close();
}

bool AtClient::is_connected() const
{
    return port_.is_open();
}

bool AtClient::isCollidingResponseLine(const std::string& line) {
    // 仅当正在执行同族 AT 命令（查询/设置）时，把该 URC 前缀的行当作命令响应缓冲，
    // 避免吞掉 AT+CEREG?/AT+CREG?/AT+Q5GREG?/AT+QUIMSLOT?/AT+QSIMSTAT? 的查询结果。
    // 注：+CREG 与 +CEREG 前缀不同（"+CREG:" 不是 "+CEREG:" 的前缀，反之亦然），
    // "AT+CREG" 也不是 "AT+CEREG" 的前缀，二者互不误匹配。
    std::string cmd;
    bool inFlight = false;
    {
        std::lock_guard<std::mutex> lk(cmd_mtx_);
        inFlight = (pending_ && !pending_->done);
        cmd = last_cmd_;
    }
    if (!inFlight || cmd.empty()) return false;
    auto starts = [](const std::string& s, const char* p) { return s.rfind(p, 0) == 0; };
    if (starts(line, "+CEREG:")         && starts(cmd, "AT+CEREG"))         return true;
    if (starts(line, "+CREG:")          && starts(cmd, "AT+CREG"))          return true;
    if (starts(line, "+C5GREG:")        && starts(cmd, "AT+C5GREG"))        return true;
    if (starts(line, "+Q5GREG:")        && starts(cmd, "AT+Q5GREG"))        return true;
    if (starts(line, "+QUIMSLOT:")      && starts(cmd, "AT+QUIMSLOT"))      return true;
    if (starts(line, "+QSIMSTAT:")      && starts(cmd, "AT+QSIMSTAT"))      return true;
    if (starts(line, "+CPIN:")          && starts(cmd, "AT+CPIN"))          return true;
    if (starts(line, "+CFUN:")          && starts(cmd, "AT+CFUN"))          return true;
    if (starts(line, "+QIND:")          && starts(cmd, "AT+QIND"))          return true;
    if (starts(line, "+QNETDEVSTATUS:") && starts(cmd, "AT+QNETDEVSTATUS")) return true;
    return false;
}

void AtClient::reader_loop()
{
    using namespace std::chrono_literals;
    while (reading_) {
        auto chunk = port_.read_some(200ms);
        if (chunk.empty())
            continue;
        acc_.feed(chunk);

        while (acc_.has_line()) {
            const auto line = acc_.next_line();
            if (line.empty())
                continue;

            // [调试] 打印模组从串口输出的每一行原始数据（回显/响应/URC/RDY/OK 等）
            // 用 LogDebug，可通过环境变量 TANGO_LOG_LEVEL 控制是否输出（1=debug 显示，2=info 隐藏）
            LogDebug << "[RX ttyUSB2] " << line;

            // 1) Final result code -> wake the waiting command().
            //    非OK终行原文保留到 err（CMEE=2 时为 verbose 文本，供故障定界）。
            if (is_final_result(line)) {
                std::lock_guard<std::mutex> lk(cmd_mtx_);
                if (pending_) {
                    pending_->done = true;
                    pending_->ok = (line == "OK");
                    if (!pending_->ok) pending_->err = line;
                    pending_->cv.notify_one();
                }
                continue;
            }

            // 2) Registered URC -> strip from response, dispatch async.
            //    与 AT 查询响应前缀冲突的 URC（+CEREG/+CREG/+Q5GREG/+QUIMSLOT/+QSIMSTAT）
            //    在执行同族命令时当响应缓冲，避免吞掉查询结果。
            if (urc_.is_urc(line) && !isCollidingResponseLine(line)) {
                // URC 高亮：模组主动上报（非查询响应），定界第 0 层证据的原始来源。
                // 用 INFO 级 + [URC] 标签与普通 [RX] 区分——默认日志级别(2)下 URC 仍可见，
                // 控制台按 INFO 颜色渲染，grep '\[URC' 可直接抽取事件时间线。
                // 注意：与查询同前缀的"伪 URC"（isCollidingResponseLine 命中）不在此列，
                // 它们是命令响应，仍走 [RX]。
                LogInfo << "[URC ttyUSB2] " << line;
                {
                    // 原始行入缓存（diag.urc 上报）：在派发给 handler 之前追加，
                    // 与 handler 是否注册/是否处理无关
                    std::lock_guard<std::mutex> lk(urc_log_mtx_);
                    if (urc_log_.size() >= kUrcLogCap)
                        urc_log_.pop_front();
                    urc_log_.push_back(line);
                }
                urc_.dispatch(line);
                continue;
            }

            // 3) Drop command echo (double safety alongside ATE0). Compare
            //    against a locked snapshot of last_cmd_ to avoid a data race
            //    on the std::string.
            std::string last;
            {
                std::lock_guard<std::mutex> lk(cmd_mtx_);
                last = last_cmd_;
            }
            if (!last.empty() && line == last)
                continue;

            // 4) Plain response data line -> buffer for current command.
            {
                std::lock_guard<std::mutex> lk(cmd_mtx_);
                if (pending_)
                    pending_->lines.push_back(line);
                else
                    LogDebug << "at stray line: " << line;
            }
        }
    }
}

AtResponse AtClient::command(const std::string& cmd, std::chrono::milliseconds timeout)
{
    AtResponse rsp;
    if (!port_.is_open())
        return rsp;

    // Serialize: wait until no other command is in flight, or shutdown.
    {
        std::unique_lock<std::mutex> lk(cmd_mtx_);
        cmd_serial_cv_.wait(lk, [this]{ return !cmd_in_flight_ || shutting_down_; });
        if (shutting_down_)
            return rsp;
        cmd_in_flight_ = true;
        pending_ = std::make_unique<PendingCmd>();
        last_cmd_ = cmd;
    }

    // Write outside the lock: write() may block and holding cmd_mtx_ here would
    // stall the reader's echo-drop / data-buffer path.
    // [调试] 打印发给模组的 AT 指令（TANGO_LOG_LEVEL=1 debug 可见，=2 info 隐藏）
    LogDebug << "[TX ttyUSB2] " << cmd;
    if (!port_.write(cmd + "\r\n")) {
        std::lock_guard<std::mutex> lk(cmd_mtx_);
        pending_.reset();
        cmd_in_flight_ = false;
        cmd_serial_cv_.notify_one();
        return rsp;
    }

    std::unique_lock<std::mutex> lk(cmd_mtx_);
    // pending_ is still valid: no other command() can replace it (serialization).
    pending_->cv.wait_for(lk, timeout, [this]{ return pending_->done || shutting_down_; });
    if (pending_) {
        rsp.ok = pending_->ok;
        rsp.lines = std::move(pending_->lines);
        rsp.err = std::move(pending_->err);
        pending_.reset();
    }
    cmd_in_flight_ = false;
    cmd_serial_cv_.notify_one();
    // Known limitation (timeout path only): if this command timed out, the
    // module may still emit its late response lines + final result. Because
    // serialization has already released the slot, a subsequent command() could
    // see those late lines bleed into its response. Timeouts are an abnormal
    // path (module unresponsive); the original synchronous implementation had
    // the same residual-buffer behavior, so this is accepted for v1.
    return rsp;
}

void AtClient::register_urc_handler(const std::string& prefix, UrcCallback cb)
{
    urc_.register_handler(prefix, std::move(cb));
}

std::vector<std::string> AtClient::drain_urc_log()
{
    std::lock_guard<std::mutex> lk(urc_log_mtx_);
    std::vector<std::string> out(urc_log_.begin(), urc_log_.end());
    urc_log_.clear();
    return out;
}

void AtClient::unregister_urc_handler(const std::string& prefix)
{
    urc_.unregister(prefix);
}

// ============== Basic Queries ==============

std::optional<std::string> AtClient::get_imei()
{
    auto r = command("AT+CGSN", std::chrono::milliseconds(1500));
    if (!r.ok) {
        r = command("AT+CGSN=1", std::chrono::milliseconds(1500));
        if (!r.ok)
            return std::nullopt;
    }
    return first_nonempty_line(r.lines);
}

std::optional<std::string> AtClient::get_imsi()
{
    auto r = command("AT+CIMI", std::chrono::milliseconds(1500));
    if (!r.ok)
        return std::nullopt;
    return first_nonempty_line(r.lines);
}

std::optional<std::string> AtClient::get_iccid()
{
    auto r = command("AT+ICCID", std::chrono::milliseconds(1500));
    if (!r.ok) {
        r = command("AT+QCCID", std::chrono::milliseconds(1500));
        if (!r.ok)
            return std::nullopt;
    }

    for (const auto& l : r.lines) {
        if (starts_with(l, "+ICCID:")) {
            return trim(l.substr(std::string("+ICCID:").size()));
        }
        if (starts_with(l, "+QCCID:")) {
            return trim(l.substr(std::string("+QCCID:").size()));
        }
    }

    return first_nonempty_line(r.lines);
}

std::optional<int> AtClient::get_csq_rssi()
{
    auto r = command("AT+CSQ", std::chrono::milliseconds(1500));
    if (!r.ok)
        return std::nullopt;
    for (const auto& l : r.lines) {
        if (starts_with(l, "+CSQ:")) {
            auto rest = trim(l.substr(std::string("+CSQ:").size()));
            auto comma = rest.find(',');
            auto rssi_str = trim(rest.substr(0, comma));
            return parse_int(rssi_str);
        }
    }
    return std::nullopt;
}

std::optional<std::string> AtClient::get_operator()
{
    auto r = command("AT+COPS?", std::chrono::milliseconds(2000));
    if (!r.ok)
        return std::nullopt;
    for (const auto& l : r.lines) {
        if (starts_with(l, "+COPS:"))
            return l;
    }
    return std::nullopt;
}

std::optional<std::string> AtClient::get_cereg()
{
    auto r = command("AT+CEREG?", std::chrono::milliseconds(1500));
    if (!r.ok)
        return std::nullopt;
    for (const auto& l : r.lines) {
        if (starts_with(l, "+CEREG:"))
            return l;
    }
    return std::nullopt;
}

// ============== Module Information ==============

std::optional<ModuleInfo> AtClient::get_module_info()
{
    ModuleInfo info;

    // AT+CGMI - Manufacturer
    auto r = command("AT+CGMI", std::chrono::milliseconds(1500));
    if (r.ok) {
        auto m = first_nonempty_line(r.lines);
        if (m) info.manufacturer = *m;
    }

    // AT+CGMM - Model
    r = command("AT+CGMM", std::chrono::milliseconds(1500));
    if (r.ok) {
        auto m = first_nonempty_line(r.lines);
        if (m) info.model = *m;
    }

    // AT+CGMR - Revision
    r = command("AT+CGMR", std::chrono::milliseconds(1500));
    if (r.ok) {
        for (const auto& l : r.lines) {
            if (starts_with(l, "+CGMR:")) {
                info.revision = trim(l.substr(std::string("+CGMR:").size()));
                break;
            }
            if (l != "OK" && !l.empty()) {
                info.revision = l;
                break;
            }
        }
    }

    // AT+CGSN - IMEI
    auto imei = get_imei();
    if (imei) info.imei = *imei;

    // AT+QCCID or AT+GSN for SN
    r = command("AT+GSN", std::chrono::milliseconds(1500));
    if (r.ok) {
        auto sn = first_nonempty_line(r.lines);
        if (sn && sn->length() >= 10) {
            info.sn = *sn;
        }
    }

    info.valid = !info.manufacturer.empty() || !info.model.empty();
    return info.valid ? std::optional<ModuleInfo>{info} : std::nullopt;
}

// ============== SIM Card ==============

std::optional<SimStatus> AtClient::get_sim_status(bool fetch_ids)
{
    SimStatus status;

    // AT+CPIN? - SIM PIN status
    auto r = command("AT+CPIN?", std::chrono::milliseconds(1500));
    if (r.ok) {
        for (const auto& l : r.lines) {
            if (starts_with(l, "+CPIN:")) {
                auto result = trim(l.substr(std::string("+CPIN:").size()));
                if (result == "READY") {
                    status.status = 0;
                    status.ready = true;
                } else if (result.find("SIM PIN") != std::string::npos) {
                    status.status = 1;
                    status.ready = false;
                } else if (result.find("SIM PUK") != std::string::npos) {
                    status.status = 2;
                    status.ready = false;
                } else {
                    status.status = 3;
                    status.ready = false;
                }
                break;
            }
        }
    }

    // IMSI/ICCID 为静态值，调用方可传 fetch_ids=false 跳过（由上层缓存复用）
    if (fetch_ids) {
        status.imsi = get_imsi().value_or("");
        status.iccid = get_iccid().value_or("");
    }

    return status;
}

bool AtClient::sim_pin_unlock(const std::string& pin)
{
    auto r = command("AT+CPIN=\"" + pin + "\"", std::chrono::milliseconds(3000));
    return r.ok;
}

// ============== Dual SIM Management ==============

bool AtClient::is_dual_sim_supported()
{
    // Try Quectel dual SIM command: AT+QUIMSLOT?
    // Response format: +QUIMSLOT: <slot>,<status>
    // If returns two lines (slot 1 and slot 2), dual SIM is supported
    // If returns only one line (slot 1), single SIM only
    auto r = command("AT+QUIMSLOT?", std::chrono::milliseconds(1500));
    if (!r.ok) {
        LogInfo << "AT+QUIMSLOT? command failed, dual sim not supported";
        return false;
    }

    // Count how many QUIMSLOT entries are returned
    int slot_count = 0;
    for (const auto& l : r.lines) {
        if (starts_with(l, "+QUIMSLOT:")) {
            slot_count++;
        }
    }

    // Dual SIM: two slots (1 and 2) detected
    // Single SIM: only one slot (1) detected
    if (slot_count >= 2) {
        LogInfo << "Dual SIM supported: detected " << slot_count << " slots";
        return true;
    } else if (slot_count == 1) {
        LogInfo << "Single SIM only: detected 1 slot";
        return false;
    }

    LogInfo << "Dual SIM not supported: no valid QUIMSLOT response";
    return false;
}

bool AtClient::select_sim_slot(int slot)
{
    if (slot < 0 || slot > 1) {
        return false;
    }

    // Quectel: AT+QUIMSLOT=<slot> where slot is 1 or 2 (1-indexed in AT command)
    int at_slot = slot + 1;  // Convert 0-indexed to 1-indexed
    auto r = command("AT+QUIMSLOT=" + std::to_string(at_slot), std::chrono::milliseconds(1500));
    return r.ok;
}

int AtClient::get_active_sim_slot()
{
    auto r = command("AT+QUIMSLOT?", std::chrono::milliseconds(1500));
    if (!r.ok) {
        return -1;
    }

    for (const auto& l : r.lines) {
        if (starts_with(l, "+QUIMSLOT:")) {
            auto parts = split_csv(l.substr(std::string("+QUIMSLOT:").size()));
            if (!parts.empty()) {
                int slot = parse_int(parts[0]).value_or(1);
                return slot - 1;  // Convert 1-indexed to 0-indexed
            }
        }
    }

    return 0;  // Default to slot 0 if not specified
}

std::optional<DualSimStatus> AtClient::get_dual_sim_status()
{
    DualSimStatus dual_status;

    // Check if dual SIM is supported
    dual_status.dual_sim_supported = is_dual_sim_supported();
    if (!dual_status.dual_sim_supported) {
        // Single SIM mode - just get the one SIM status
        auto status = get_sim_status();
        if (status) {
            dual_status.sim[0] = *status;
            dual_status.sim[0].sim_slot = 0;
            dual_status.sim[0].present = status->ready || !status->iccid.empty();
        }
        dual_status.active_slot = 0;
        dual_status.valid = true;
        return dual_status;
    }

    // Dual SIM mode - query each slot
    int original_slot = get_active_sim_slot();

    // Get SIM status for slot 0
    if (select_sim_slot(0)) {
        auto status = get_sim_status();
        if (status) {
            dual_status.sim[0] = *status;
            dual_status.sim[0].sim_slot = 0;
            dual_status.sim[0].present = status->ready || !status->iccid.empty();
        }

        // Also check AT+QSIMSTAT for slot presence
        auto r = command("AT+QSIMSTAT?", std::chrono::milliseconds(1500));
        if (r.ok) {
            for (const auto& l : r.lines) {
                // +QSIMSTAT: <slot>,<status>
                if (starts_with(l, "+QSIMSTAT:")) {
                    auto parts = split_csv(l.substr(std::string("+QSIMSTAT:").size()));
                    if (parts.size() >= 2) {
                        int slot_idx = parse_int(parts[0]).value_or(0);
                        int sim_stat = parse_int(parts[1]).value_or(0);
                        if (slot_idx == 1) {  // AT uses 1-indexed
                            dual_status.sim[0].present = (sim_stat == 1);
                        } else if (slot_idx == 2) {
                            dual_status.sim[1].present = (sim_stat == 1);
                        }
                    }
                }
            }
        }
    }

    // Get SIM status for slot 1
    if (select_sim_slot(1)) {
        auto status = get_sim_status();
        if (status) {
            dual_status.sim[1] = *status;
            dual_status.sim[1].sim_slot = 1;
            dual_status.sim[1].present = status->ready || !status->iccid.empty();
        }
    }

    // Restore original slot
    if (original_slot >= 0) {
        select_sim_slot(original_slot);
    }

    dual_status.active_slot = get_active_sim_slot();
    dual_status.valid = true;
    return dual_status;
}

std::optional<SimStatus> AtClient::get_sim_status_slot(int slot)
{
    if (slot < 0 || slot > 1) {
        return std::nullopt;
    }

    // Remember current slot
    int original_slot = get_active_sim_slot();

    // Switch to requested slot
    if (!select_sim_slot(slot)) {
        return std::nullopt;
    }

    // Get SIM status
    auto status = get_sim_status();
    if (status) {
        status->sim_slot = slot;
    }

    // Restore original slot
    if (original_slot >= 0 && original_slot != slot) {
        select_sim_slot(original_slot);
    }

    return status;
}

// ============== Network Registration ==============

std::optional<NetworkRegStatus> AtClient::get_gsm_registration()
{
    auto r = command("AT+CREG?", std::chrono::milliseconds(1500));
    if (!r.ok)
        return std::nullopt;

    for (const auto& l : r.lines) {
        if (starts_with(l, "+CREG:")) {
            NetworkRegStatus status;
            auto parts = split_csv(l.substr(std::string("+CREG:").size()));
            if (parts.size() >= 2) {
                status.stat = parse_int(parts[1]).value_or(0);
                if (parts.size() >= 3) status.rac = parts[2];
                if (parts.size() >= 4) status.tac = parts[3];
                if (parts.size() >= 5) status.ci = parts[4];
                status.valid = true;
                return status;
            }
        }
    }
    return std::nullopt;
}

std::optional<NetworkRegStatus> AtClient::get_lte_registration()
{
    auto r = command("AT+CEREG?", std::chrono::milliseconds(1500));
    if (!r.ok)
        return std::nullopt;

    for (const auto& l : r.lines) {
        if (starts_with(l, "+CEREG:")) {
            NetworkRegStatus status;
            auto parts = split_csv(l.substr(std::string("+CEREG:").size()));
            if (parts.size() >= 2) {
                status.stat = parse_int(parts[1]).value_or(0);
                if (parts.size() >= 3) status.tac = parts[2];
                if (parts.size() >= 4) status.ci = parts[3];
                if (parts.size() >= 5) status.act = parse_int(parts[4]).value_or(0);
                status.valid = true;
                return status;
            }
        }
    }
    return std::nullopt;
}

std::optional<NetworkRegStatus> AtClient::get_5g_registration()
{
    // Quectel specific: AT+Q5GREG?
    auto r = command("AT+Q5GREG?", std::chrono::milliseconds(1500));
    if (!r.ok)
        return std::nullopt;

    for (const auto& l : r.lines) {
        if (starts_with(l, "+Q5GREG:")) {
            NetworkRegStatus status;
            auto parts = split_csv(l.substr(std::string("+Q5GREG:").size()));
            if (parts.size() >= 2) {
                status.stat = parse_int(parts[1]).value_or(0);
                status.act = 5; // NR
                status.valid = true;
                return status;
            }
        }
    }
    return std::nullopt;
}

// ============== Signal Quality ==============

std::optional<LteSignalInfo> AtClient::get_lte_signal()
{
    // Try AT+CESQ first (3GPP standard)
    // Response: +CESQ: <rxlev>,<ber>,<rscp>,<ecn0>,<rsrq>,<rsrp>
    auto r = command("AT+CESQ", std::chrono::milliseconds(2000));
    if (r.ok) {
        for (const auto& l : r.lines) {
            if (!starts_with(l, "+CESQ:"))
                continue;

            auto parts = split_csv(l.substr(std::string("+CESQ:").size()));
            // +CESQ: rxlev,ber,rscp,ecn0,rsrq,rsrp
            if (parts.size() >= 6) {
                LteSignalInfo info;
                auto rxlev_opt = parse_int(parts[0]);
                auto rsrq_val_opt = parse_int(parts[4]);
                auto rsrp_val_opt = parse_int(parts[5]);

                // Convert RSRP: 0=-140dBm, 1=-139dBm, ..., 97=-44dBm, 255=unknown
                if (rsrp_val_opt && *rsrp_val_opt != 255 && *rsrp_val_opt <= 97) {
                    info.rsrp = -140 + *rsrp_val_opt;
                    info.valid = true;
                }

                // Convert RSRQ: 0=-19.5dB, 1=-19dB, ..., 34=-3dB
                if (rsrq_val_opt && *rsrq_val_opt != 255 && *rsrq_val_opt <= 34) {
                    info.rsrq = static_cast<int>(-19.5 + *rsrq_val_opt * 0.5);
                }

                // Convert RSSI from rxlev: 0=-110dBm, ..., 63=-47dBm
                if (rxlev_opt && *rxlev_opt != 99 && *rxlev_opt <= 63) {
                    info.rssi = -110 + *rxlev_opt;
                }

                if (info.valid)
                    return info;
            }
        }
    }

    // Fallback: Quectel specific AT+QCSQ
    // Response: +QCSQ: "LTE",<rssi>,<rsrp>,<sinr>,<rsrq>
    r = command("AT+QCSQ", std::chrono::milliseconds(2000));
    if (r.ok) {
        for (const auto& l : r.lines) {
            if (!starts_with(l, "+QCSQ:"))
                continue;

            auto parts = split_csv(l.substr(std::string("+QCSQ:").size()));
            if (parts.size() >= 5) {
                // Check for LTE mode
                std::string mode = trim(parts[0]);
                if (mode == "\"LTE\"" || mode == "LTE") {
                    LteSignalInfo info;
                    auto rssi_opt = parse_int(parts[1]);
                    auto rsrp_opt = parse_int(parts[2]);
                    auto sinr_opt = parse_int(parts[3]);
                    auto rsrq_opt = parse_int(parts[4]);

                    if (rsrp_opt) {
                        info.rssi = rssi_opt.value_or(-999);
                        info.rsrp = *rsrp_opt;
                        info.sinr = sinr_opt.value_or(-999);
                        info.rsrq = rsrq_opt.value_or(-999);
                        info.valid = true;
                        return info;
                    }
                }
            }
        }
    }

    // Final fallback: AT+CSQ (only RSSI)
    auto rssi_opt = get_csq_rssi();
    if (rssi_opt && *rssi_opt != 99) {
        LteSignalInfo info;
        info.rssi = -113 + (*rssi_opt * 2);
        info.rsrp = info.rssi + 10; // Rough estimate
        info.valid = true;
        return info;
    }

    return std::nullopt;
}

// 一次 AT+QENG="servingcell" 查询，同时解析 NR 信号与服务小区
// 供 get_nr_signal / get_serving_cell 共享同一次 AT 查询，避免重复下发
ServingCellInfo AtClient::get_servingcell_info()
{
    ServingCellInfo result;
    auto r = command("AT+QENG=\"servingcell\"", std::chrono::milliseconds(2000));
    if (!r.ok)
        return result;

    // 打印模组对 AT+QENG="servingcell" 的原始回复，便于核对 NR5G 字段列序
    for (const auto& l : r.lines) {
        LogInfo << "[QENG servingcell raw] " << l;
    }

    bool cell_taken = false;  // 服务小区只取第一个 servingcell 行（与原 get_serving_cell 行为一致）
    for (const auto& l : r.lines) {
        if (!starts_with(l, "+QENG:"))
            continue;

        auto parts = split_csv(l.substr(std::string("+QENG:").size()));
        if (parts.size() < 3)
            continue;

        std::string type = trim(parts[0]);
        if (type != "\"servingcell\"" && type != "servingcell")
            continue;

        std::string mode = trim(parts[2]);

        // ---- 服务小区：仅第一个 servingcell 行 ----
        // LTE:  +QENG: "servingcell","NOCONN","LTE",<mcc>,<mnc>,<cellid>,<pci>,<earfcn>,<freq>,<band>,...
        // NR5G: +QENG: "servingcell","NOCONN","NR5G-SA","TDD",<mcc>,<mnc>,<cellID>,<pcid>,<tac>,<arfcn>,<band>,<bw>,<rsrp>,<rsrq>,<sinr>,<srxlev>,<scs>
        // GSM:  +QENG: "servingcell","NOCONN","GSM",...
        if (!cell_taken) {
            cell_taken = true;
            CellInfo info;
            if (mode.find("LTE") != std::string::npos && parts.size() >= 9) {
                info.mcc = trim(parts[3]);
                info.mnc = trim(parts[4]);
                info.cell_id = trim(parts[5]);
                info.pci = trim(parts[6]);
                info.earfcn = trim(parts[7]);
                info.band = trim(parts[9]);
                info.valid = true;
            } else if (mode.find("NR5G") != std::string::npos && parts.size() >= 9) {
                // parts[4]=mcc, [5]=mnc, [6]=cellID(十六进制), [7]=pcid, [8]=tac(十六进制)
                info.mcc = trim(parts[4]);
                info.mnc = trim(parts[5]);
                info.cell_id = trim(parts[6]);
                info.pci = trim(parts[7]);
                info.tac = trim(parts[8]);
                info.valid = true;
            } else if (mode.find("GSM") != std::string::npos && parts.size() >= 8) {
                info.mcc = trim(parts[3]);
                info.mnc = trim(parts[4]);
                info.cell_id = trim(parts[5]);
                info.tac = trim(parts[7]); // LAC for GSM
                info.valid = true;
            }
            if (info.valid)
                result.cell = info;
        }

        // ---- NR 信号：NR5G 行解析 rsrp/rsrq/sinr（parts[12/13/14]，已为 dBm/dB）----
        if (mode.find("NR5G") != std::string::npos && !result.nr_signal && parts.size() >= 15) {
            auto rsrp_opt = parse_int(parts[12]);
            auto rsrq_opt = parse_int(parts[13]);
            auto sinr_opt = parse_int(parts[14]);

            // RSRP 为 dBm 负值；过滤 0/无效占位，避免误报
            if (rsrp_opt && *rsrp_opt < 0) {
                NrSignalInfo ns;
                ns.rsrp = *rsrp_opt;
                ns.rsrq = rsrq_opt.value_or(-999);
                ns.sinr = sinr_opt.value_or(-999);
                ns.valid = true;
                result.nr_signal = ns;
            }
        }
    }

    return result;
}

std::optional<NrSignalInfo> AtClient::get_nr_signal()
{
    return get_servingcell_info().nr_signal;
}

// ============== Cell Information ==============

std::optional<CellInfo> AtClient::get_serving_cell()
{
    return get_servingcell_info().cell;
}

std::optional<CellInfo> AtClient::get_lte_cell_info()
{
    // AT+QENG="servingcell" for LTE
    auto r = command("AT+QENG=\"servingcell\"", std::chrono::milliseconds(2000));
    if (!r.ok)
        return std::nullopt;

    for (const auto& l : r.lines) {
        if (!starts_with(l, "+QENG:"))
            continue;

        auto parts = split_csv(l.substr(std::string("+QENG:").size()));
        if (parts.size() >= 10) {
            std::string mode = trim(parts[2]);
            if (mode.find("LTE") != std::string::npos) {
                CellInfo info;
                info.mcc = trim(parts[3]);
                info.mnc = trim(parts[4]);
                info.cell_id = trim(parts[5]);
                info.pci = trim(parts[6]);
                info.earfcn = trim(parts[7]);
                info.freq = parse_int(parts[8]).value_or(0);
                info.band = trim(parts[9]);
                info.valid = true;
                return info;
            }
        }
    }

    return std::nullopt;
}

std::optional<CellInfo> AtClient::get_nr_cell_info()
{
    auto r = command("AT+QENG=\"servingcell\"", std::chrono::milliseconds(2000));
    if (!r.ok)
        return std::nullopt;

    for (const auto& l : r.lines) {
        if (!starts_with(l, "+QENG:"))
            continue;

        auto parts = split_csv(l.substr(std::string("+QENG:").size()));
        if (parts.size() >= 7) {
            std::string mode = trim(parts[2]);
            if (mode.find("NR5G") != std::string::npos) {
                CellInfo info;
                info.mcc = trim(parts[3]);
                info.mnc = trim(parts[4]);
                info.cell_id = trim(parts[5]);
                info.pci = trim(parts[6]);
                if (parts.size() >= 8) info.earfcn = trim(parts[7]);
                info.valid = true;
                return info;
            }
        }
    }

    return std::nullopt;
}

// ============== Network Selection ==============

bool AtClient::set_auto_network()
{
    auto r = command("AT+COPS=0", std::chrono::milliseconds(30000)); // 30s timeout
    return r.ok;
}

bool AtClient::set_network_mode(int mode)
{
    // AT+CNMP: 2=Auto, 13=GSM only, 38=LTE only, 71=NR only, etc.
    auto r = command("AT+CNMP=" + std::to_string(mode), std::chrono::milliseconds(5000));
    if (r.ok)
        return true;

    // Fallback: AT+QCFG="nwscanmode"
    r = command("AT+QCFG=\"nwscanmode\"," + std::to_string(mode), std::chrono::milliseconds(5000));
    return r.ok;
}

int AtClient::get_network_mode()
{
    auto r = command("AT+CNMP?", std::chrono::milliseconds(1500));
    if (r.ok) {
        for (const auto& l : r.lines) {
            if (starts_with(l, "+CNMP:")) {
                auto mode = parse_int(trim(l.substr(std::string("+CNMP:").size())));
                return mode.value_or(0);
            }
        }
    }

    // Fallback
    r = command("AT+QCFG=\"nwscanmode\"", std::chrono::milliseconds(1500));
    if (r.ok) {
        for (const auto& l : r.lines) {
            if (starts_with(l, "+QCFG:")) {
                auto parts = split_csv(l.substr(std::string("+QCFG:").size()));
                if (parts.size() >= 1) {
                    return parse_int(trim(parts[0])).value_or(0);
                }
            }
        }
    }

    return 0;
}

// ============== GPS/GNSS ==============

bool AtClient::gps_enable(int mode)
{
    // AT+QGPS=<mode>: 1=standalone, 2=MS-based, 3=MS-assisted
    auto r = command("AT+QGPS=" + std::to_string(mode), std::chrono::milliseconds(3000));
    return r.ok;
}

bool AtClient::gps_disable()
{
    auto r = command("AT+QGPS=0", std::chrono::milliseconds(3000));
    return r.ok;
}

bool AtClient::gps_set_mode(int gnss_mode)
{
    // AT+QGPSGNSSCFG=<gnss_mode>: bitmask of GPS, GLONASS, BDS, GALILEO, etc.
    auto r = command("AT+QGPSGNSSCFG=" + std::to_string(gnss_mode), std::chrono::milliseconds(3000));
    return r.ok;
}

std::optional<GpsLocation> AtClient::get_location()
{
    // Try AT+CGPSINFO first
    auto r = command("AT+CGPSINFO", std::chrono::milliseconds(2000));

    auto parse_cgps = [](const std::vector<std::string>& lines) -> std::optional<GpsLocation> {
        for (const auto& l : lines) {
            if (!starts_with(l, "+CGPSINFO:"))
                continue;

            auto parts = split_csv(l.substr(std::string("+CGPSINFO:").size()));
            if (parts.size() < 4)
                return std::nullopt;

            auto conv = [](const std::string& dm) -> std::optional<double> {
                if (dm.empty() || dm == "-")
                    return std::nullopt;
                if (dm.size() < 4)
                    return std::nullopt;
                auto dot = dm.find('.');
                auto head_len = (dot == std::string::npos) ? dm.size() - 2 : dot - 2;
                if (head_len <= 0 || head_len > dm.size())
                    return std::nullopt;
                try {
                    double deg = std::stod(dm.substr(0, head_len));
                    double mins = std::stod(dm.substr(head_len));
                    return deg + mins / 60.0;
                } catch (...) {
                    return std::nullopt;
                }
            };

            auto lat_dm = parts[0];
            auto lat_ns = parts[1];
            auto lon_dm = parts[2];
            auto lon_ew = parts[3];

            if (lat_dm.empty() || lat_dm == "-" || lon_dm.empty() || lon_dm == "-")
                return std::nullopt;

            auto lat_opt = conv(lat_dm);
            auto lon_opt = conv(lon_dm);
            if (!lat_opt || !lon_opt)
                return std::nullopt;

            GpsLocation loc;
            loc.lat = *lat_opt;
            loc.lon = *lon_opt;
            if (lat_ns == "S") loc.lat = -loc.lat;
            if (lon_ew == "W") loc.lon = -loc.lon;

            // Parse additional fields if available
            if (parts.size() >= 5) loc.timestamp = parts[4];
            if (parts.size() >= 6) loc.altitude = parse_int(parts[5]).value_or(0);

            loc.valid = true;
            return loc;
        }
        return std::nullopt;
    };

    if (r.ok) {
        auto loc = parse_cgps(r.lines);
        if (loc)
            return loc;
    }

    // Fallback: AT+QGPSLOC
    r = command("AT+QGPSLOC=2", std::chrono::milliseconds(2000));
    if (!r.ok)
        return std::nullopt;

    for (const auto& l : r.lines) {
        if (!starts_with(l, "+QGPSLOC:"))
            continue;

        auto parts = split_csv(l.substr(std::string("+QGPSLOC:").size()));
        if (parts.size() < 3)
            return std::nullopt;

        try {
            GpsLocation loc;
            loc.timestamp = parts[0];
            loc.lat = std::stod(parts[1]);
            loc.lon = std::stod(parts[2]);

            if (parts.size() >= 4) loc.altitude = std::stod(parts[3]);
            if (parts.size() >= 5) loc.speed = std::stod(parts[4]);
            if (parts.size() >= 6) loc.course = std::stod(parts[5]);

            loc.valid = true;
            return loc;
        } catch (...) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

// ============== PDP Context ==============

bool AtClient::define_pdp_context(int cid, const std::string& apn)
{
    // AT+CGDCONT=<cid>,<PDP_type>,<APN>
    auto r = command("AT+CGDCONT=" + std::to_string(cid) + ",\"IP\",\"" + apn + "\"",
                     std::chrono::milliseconds(3000));
    return r.ok;
}

bool AtClient::activate_pdp_context(int cid)
{
    // AT+CGACT=<state>,<cid>
    auto r = command("AT+CGACT=1," + std::to_string(cid), std::chrono::milliseconds(10000));
    return r.ok;
}

bool AtClient::deactivate_pdp_context(int cid)
{
    auto r = command("AT+CGACT=0," + std::to_string(cid), std::chrono::milliseconds(5000));
    return r.ok;
}

std::string AtClient::get_pdp_address(int cid)
{
    auto r = command("AT+CGPADDR=" + std::to_string(cid), std::chrono::milliseconds(1500));
    if (!r.ok)
        return "";

    for (const auto& l : r.lines) {
        if (starts_with(l, "+CGPADDR:")) {
            auto parts = split_csv(l.substr(std::string("+CGPADDR:").size()));
            if (parts.size() >= 2) {
                return trim(parts[1]);
            }
        }
    }
    return "";
}

// ============== Network Time ==============

std::optional<std::string> AtClient::get_network_time()
{
    // Try AT+QLTS (Quectel specific)
    auto r = command("AT+QLTS", std::chrono::milliseconds(2000));
    if (r.ok) {
        for (const auto& l : r.lines) {
            if (starts_with(l, "+QLTS:")) {
                return trim(l.substr(std::string("+QLTS:").size()));
            }
        }
    }

    // Fallback: AT+CCLK?
    r = command("AT+CCLK?", std::chrono::milliseconds(1500));
    if (r.ok) {
        for (const auto& l : r.lines) {
            if (starts_with(l, "+CCLK:")) {
                return trim(l.substr(std::string("+CCLK:").size()));
            }
        }
    }

    return std::nullopt;
}

// ============== Reset/Power ==============

bool AtClient::soft_reset()
{
    // AT+CFUN=0 then AT+CFUN=1
    auto r = command("AT+CFUN=0", std::chrono::milliseconds(10000));
    if (!r.ok)
        return false;

    // Wait a bit before re-enabling
    r = command("AT+CFUN=1", std::chrono::milliseconds(15000));
    return r.ok;
}

bool AtClient::set_radio_function(int mode)
{
    // AT+CFUN=<mode>: 0=minimum, 1=full, 4=disable RF
    auto r = command("AT+CFUN=" + std::to_string(mode), std::chrono::milliseconds(10000));
    return r.ok;
}

} // namespace tbox