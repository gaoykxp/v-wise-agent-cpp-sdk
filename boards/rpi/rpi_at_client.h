#pragma once

#include "rpi_serial_port.h"
#include "rpi_urc.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tbox {

struct AtResponse {
    bool ok{false};
    std::vector<std::string> lines; // trimmed data lines (echo/URC/final result excluded)
};

struct GpsLocation {
    double lat{0.0};
    double lon{0.0};
    double altitude{0.0};
    double speed{0.0};
    double course{0.0};
    std::string timestamp;
    bool valid{false};
};

// LTE signal information
struct LteSignalInfo {
    int rsrp{-999};       // dBm, range: -140 to -44
    int rsrq{-999};       // dB, range: -20 to -3
    int sinr{-999};       // dB, range: -21 to 30
    int rssi{-999};       // dBm, range: -100 to -50
    int snr{-999};        // dB, for some modules
    bool valid{false};
};

// 5G NR signal information
struct NrSignalInfo {
    int rsrp{-999};       // dBm
    int rsrq{-999};       // dB
    int sinr{-999};       // dB
    int snr{-999};        // dB
    bool valid{false};
};

// Cell information
struct CellInfo {
    std::string mcc;           // Mobile Country Code
    std::string mnc;           // Mobile Network Code
    std::string cell_id;       // Cell ID
    std::string tac;           // Tracking Area Code (LTE) or LAC (GSM)
    std::string pci;           // Physical Cell ID (LTE/NR)
    std::string earfcn;        // E-UTRA Absolute Radio Frequency Channel Number
    std::string band;          // Frequency band
    int freq{0};               // Frequency in kHz
    int bandwidth{0};          // Bandwidth in MHz
    bool valid{false};
};

// 一次 AT+QENG="servingcell" 查询的合并结果：NR 信号 + 服务小区
// 供 get_nr_signal / get_serving_cell 共享同一次 AT 查询，避免重复下发
struct ServingCellInfo {
    std::optional<NrSignalInfo> nr_signal;
    std::optional<CellInfo> cell;
};

// Network registration status
struct NetworkRegStatus {
    int stat{0};               // 0=not registered, 1=registered home, 2=searching, 3=denied, 4=unknown, 5=roaming
    int act{0};                // Access technology: 0=GSM, 2=UTRAN, 3=LTE, 4=LTE+, 5=NR
    std::string rac;           // Routing Area Code
    std::string tac;           // Tracking Area Code
    std::string ci;            // Cell Identity
    int cause{0};              // Rejection cause
    bool valid{false};
};

// SIM card status
struct SimStatus {
    int sim_slot{0};          // SIM card slot number (0 or 1 for dual SIM)
    std::string imsi;
    std::string iccid;
    int status{0};            // 0=ready, 1=PIN required, 2=PUK required, 3=SIM busy, etc.
    bool ready{false};
    bool present{false};      // SIM card is present/inserted
};

// Dual SIM status
struct DualSimStatus {
    SimStatus sim[2];         // sim[0] for slot 0, sim[1] for slot 1
    int active_slot{0};       // Currently active SIM slot
    bool dual_sim_supported{false};
    bool valid{false};
};

// Module information
struct ModuleInfo {
    std::string manufacturer;
    std::string model;
    std::string revision;
    std::string imei;
    std::string sn;
    bool valid{false};
};

class AtClient
{
public:
    AtClient() = default;
    ~AtClient();

    bool connect(const std::string& port_name, int baud_rate = 115200);
    void disconnect();
    bool is_connected() const;

    // Send "AT+..." (without trailing CRLF) and wait for response.
    // The reader thread owns the serial port read path; command() registers a
    // pending request and waits on a condition variable until the reader sees
    // a final result (OK/ERROR/+CME/+CMS ERROR) or the timeout elapses.
    //
    // Commands are serialized: at most one command is in flight at a time. A
    // call that arrives while another is pending blocks (until the in-flight
    // one completes or times out) rather than overwriting it, so URC callbacks
    // may safely issue their own AT commands.
    AtResponse command(const std::string& cmd,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(1500));

    // ============== URC (Unsolicited Result Code) ==============
    // Register a handler for a URC prefix (e.g. "+CMT:") or bare word (e.g. "RDY").
    // Callbacks run on a dedicated worker thread, so it is safe to issue AT
    // commands from inside them. May be called before or after connect().
    void register_urc_handler(const std::string& prefix, UrcCallback cb);
    void unregister_urc_handler(const std::string& prefix);

    // ============== Basic Queries ==============
    std::optional<std::string> get_imei();
    std::optional<std::string> get_imsi();
    std::optional<std::string> get_iccid();
    std::optional<int> get_csq_rssi();          // 0..31, 99 unknown
    std::optional<std::string> get_operator();
    std::optional<std::string> get_cereg();     // LTE registration (legacy)

    // ============== Module Information ==============
    std::optional<ModuleInfo> get_module_info();

    // ============== SIM Card ==============
    std::optional<SimStatus> get_sim_status(bool fetch_ids = true);  // AT+CPIN?(+CIMI+ICCID when fetch_ids)
    bool sim_pin_unlock(const std::string& pin);

    // ============== Dual SIM Management (Quectel 5G modules) ==============
    // Check if dual SIM is supported
    bool is_dual_sim_supported();

    // Select active SIM slot (0 or 1)
    bool select_sim_slot(int slot);

    // Get currently active SIM slot (returns 0 or 1, -1 on error)
    int get_active_sim_slot();

    // Get dual SIM status (both slots at once)
    std::optional<DualSimStatus> get_dual_sim_status();

    // Get SIM status for specific slot (switches to that slot first)
    std::optional<SimStatus> get_sim_status_slot(int slot);

    // ============== Network Registration ==============
    std::optional<NetworkRegStatus> get_gsm_registration();  // AT+CREG?
    std::optional<NetworkRegStatus> get_lte_registration();  // AT+CEREG?
    std::optional<NetworkRegStatus> get_5g_registration();   // AT+Q5GREG? (Quectel specific)

    // ============== Signal Quality ==============
    std::optional<LteSignalInfo> get_lte_signal();           // RSRP, RSRQ, SINR via AT+CESQ or AT+QCSQ
    std::optional<NrSignalInfo> get_nr_signal();             // 5G NR signal via AT+QENG="servingcell"

    // ============== Cell Information ==============
    std::optional<CellInfo> get_serving_cell();              // AT+QENG="servingcell"
    ServingCellInfo get_servingcell_info();                  // 一次 AT+QENG="servingcell" 同时取 NR 信号与服务小区
    std::optional<CellInfo> get_lte_cell_info();             // LTE cell info
    std::optional<CellInfo> get_nr_cell_info();              // 5G NR cell info

    // ============== Network Selection ==============
    bool set_auto_network();
    bool set_network_mode(int mode);                         // 1=GSM, 2=LTE, 3=NR, etc.
    int get_network_mode();

    // ============== GPS/GNSS ==============
    bool gps_enable(int mode = 1);                           // AT+QGPS=1
    bool gps_disable();                                       // AT+QGPS=0
    bool gps_set_mode(int gnss_mode);                        // AT+QGPSGNSSCFG
    std::optional<GpsLocation> get_location();               // Legacy GPS location

    // ============== PDP Context ==============
    bool define_pdp_context(int cid, const std::string& apn);
    bool activate_pdp_context(int cid);
    bool deactivate_pdp_context(int cid);
    std::string get_pdp_address(int cid);

    // ============== Network Time ==============
    std::optional<std::string> get_network_time();           // AT+QLTS or AT+CCLK?

    // ============== Reset/Power ==============
    bool soft_reset();                                        // AT+CFUN=0; AT+CFUN=1
    bool set_radio_function(int mode);                       // AT+CFUN=<mode>

private:
    // Background reader loop. The ONLY consumer of port_.read_some(). It
    // classifies each complete line into: final result -> wake command();
    // registered URC -> dispatch to worker; command echo -> drop; otherwise ->
    // append to the pending command's response buffer.
    void reader_loop();

    // 判定某行是否为“与 AT 查询响应前缀冲突的 URC、且当前正在执行同族命令”：
    // 若是，应作为命令响应缓冲（不作为 URC 派发），避免吞掉 AT+CEREG?/AT+CREG?/
    // AT+Q5GREG?/AT+QUIMSLOT?/AT+QSIMSTAT? 等查询结果。
    bool isCollidingResponseLine(const std::string& line);

    SerialPort port_;

    // ---- reader thread + command coordination ----
    // cmd_mtx_ guards: pending_, last_cmd_, cmd_in_flight_, shutting_down_.
    // Command serialization uses cmd_in_flight_ + cmd_serial_cv_: a second
    // command() blocks until the in-flight one clears pending_, so concurrent
    // calls (e.g. from a URC callback) never overwrite each other's PendingCmd.
    struct PendingCmd {
        std::vector<std::string> lines;
        bool done{false};
        bool ok{false};
        std::condition_variable cv;
    };

    std::mutex                 cmd_mtx_;
    std::condition_variable    cmd_serial_cv_;  // waited on while cmd_in_flight_
    std::unique_ptr<PendingCmd> pending_;
    std::string                last_cmd_;       // for dropping command echo (ATE0 also set)
    bool                       cmd_in_flight_{false};
    bool                       shutting_down_{false};
    LineAccumulator            acc_;            // raw bytes -> trimmed lines
    UrcDispatcher              urc_;
    std::thread                reader_;
    std::atomic<bool>          reading_{false};

    static std::string trim(std::string s);
    static std::optional<std::string> first_nonempty_line(const std::vector<std::string>& lines);
    static std::vector<std::string> split_csv(const std::string& s);
    static std::optional<int> parse_int(const std::string& s);
};

} // namespace tbox