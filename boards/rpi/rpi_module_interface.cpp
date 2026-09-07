/**********************************************************************************************************************
    > File Name: rpi_module_interface.cpp
    > Author: Generated for Raspberry Pi
    > Date: 03/06/26
    > Description: Implementation for Raspberry Pi module interface with dual SIM support
**********************************************************************************************************************/
#include "rpi_module_interface.h"
#include "log.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <sys/utsname.h>

namespace rpi {

    // SimSlotGuard implementation - switches to target slot and restores on destruction
    // dual_sim=false（单卡）时为空操作，避免无谓的 AT+QUIMSLOT? 查询
    RPIModuleInterface::SimSlotGuard::SimSlotGuard(tbox::AtClient& client, int target_slot, bool dual_sim)
        : client_(client), original_slot_(target_slot), success_(true), dual_sim_(dual_sim)
    {
        if (!dual_sim_) return;  // 单卡：卡槽不会变，无需查询/切换
        original_slot_ = client_.get_active_sim_slot();
        if (original_slot_ < 0) {
            original_slot_ = 0;  // Default
        }

        if (original_slot_ != target_slot) {
            success_ = client_.select_sim_slot(target_slot);
        } else {
            success_ = true;  // Already on target slot
        }
    }

    RPIModuleInterface::SimSlotGuard::~SimSlotGuard()
    {
        if (!dual_sim_) return;  // 单卡：无需恢复
        // Restore original slot
        int current = client_.get_active_sim_slot();
        if (current >= 0 && current != original_slot_) {
            client_.select_sim_slot(original_slot_);
        }
    }

    // RPIModuleInterface implementation
    RPIModuleInterface& RPIModuleInterface::getInstance() {
        static RPIModuleInterface instance;
        return instance;
    }

    InitStatus RPIModuleInterface::init(int sim_id) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_initialized) {
            return InitStatus::ALREADY_INITIALIZED;
        }

        m_current_sim_id = sim_id;
        m_initialized = true;

        // Connect AT client if port is configured
        if (!m_atPort.empty() && !m_atClient.is_connected()) {
            if (m_atClient.connect(m_atPort, m_atBaudRate)) {
                LogInfo <<"[RPi] AT client connected to " << m_atPort << " at " << m_atBaudRate << " baud" ;

                // ---- URC setup ----
                // 注册 URC handler（仅打印）。RM520N-GL 状态 URC 一并注册：
                // +QUSIM/+QIND/+CSCON/+CGEV/+CTZV/+CTZE/+CNEC，以及与 AT 查询响应前缀
                // 冲突的注册类 +QUIMSLOT/+CEREG/+CREG/+C5GREG/+Q5GREG/+QSIMSTAT。
                // 冲突类由 reader_loop 的 isCollidingResponseLine 命令感知判定安全处理：
                // 执行同族查询时当响应缓冲、否则当 URC 派发，不会吞掉查询结果。
                m_atClient.register_urc_handler("RDY", [](const std::string& l) {
                    LogInfo <<"[RPi] URC module ready: " << l ;
                });
                m_atClient.register_urc_handler("+CFUN:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC radio function: " << l ;
                });
                m_atClient.register_urc_handler("+QSIMSTAT:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC SIM status: " << l ;
                });
                m_atClient.register_urc_handler("+CMTI:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC SMS received: " << l ;
                });
                // ---- RM520N-GL 其余 URC（仅打印）----
                m_atClient.register_urc_handler("+QUSIM:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC USIM status: " << l ;
                });
                m_atClient.register_urc_handler("+QIND:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC indication: " << l ;
                });
                // +CSCON: 0=RRC 空闲（对应 ue_state NOCONN），1=RRC 已建立（CONNECTED）
                m_atClient.register_urc_handler("+CSCON:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC RRC state: " << l ;
                });
                m_atClient.register_urc_handler("+CGEV:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC PDP event: " << l ;
                });
                m_atClient.register_urc_handler("+CTZV:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC time zone: " << l ;
                });
                m_atClient.register_urc_handler("+CTZE:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC time zone: " << l ;
                });
                m_atClient.register_urc_handler("+CNEC:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC network error: " << l ;
                });
                // ---- 与 AT 查询响应前缀冲突的注册类 URC ----
                // reader_loop 已加命令感知判定(isCollidingResponseLine)：执行同族查询时
                // 当响应缓冲、否则当 URC 派发，故可安全订阅。
                m_atClient.register_urc_handler("+QUIMSLOT:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC SIM slot: " << l ;
                });
                m_atClient.register_urc_handler("+CEREG:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC EPS reg: " << l ;
                });
                m_atClient.register_urc_handler("+CREG:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC GSM reg: " << l ;
                });
                m_atClient.register_urc_handler("+C5GREG:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC 5GS reg: " << l ;
                });
                m_atClient.register_urc_handler("+Q5GREG:", [](const std::string& l) {
                    LogInfo <<"[RPi] URC 5G reg: " << l ;
                });

                // ---- URC 使能序列（失败忽略：部分模组不支持个别指令）----
                // ATE0 已由 AtClient::connect() 首条下发。CEREG/C5GREG n=3：
                // URC 带位置 + EMM 拒绝原因（stat=3 时尾随 cause_type/reject_cause，
                // 注册被拒证据随事件直达，无需轮询）。rrcstate 经 +QIND: "rrcstate" 上报。
                const auto to = std::chrono::milliseconds(1500);
                m_atClient.command("AT+CMEE=2", to);       // 错误报告 verbose（AtResponse.err 依赖）
                m_atClient.command("AT+QSIMSTAT=1", to);   // SIM 热插拔 URC
                m_atClient.command("AT+CNMI=2,1,0,0,0", to); // 短信到达 URC
                m_atClient.command("AT+CGEREP=1", to);     // PDP 上下文事件 URC
                m_atClient.command("AT+CTZR=1", to);       // 时区 URC (+CTZE)
                m_atClient.command("AT+CEREG=3", to);      // EPS 注册 URC（带位置+拒绝原因）
                m_atClient.command("AT+CREG=2", to);       // CS 注册 URC（带位置）
                m_atClient.command("AT+C5GREG=3", to);     // 5GS 注册 URC（带位置+拒绝原因）
                m_atClient.command("AT+QINDCFG=\"rrcstate\",1", to); // RRC 状态 URC
                m_atClient.command("AT+CSCON=1", to);     // RRC 连接态 URC（+CSCON:0=空闲 NOCONN，1=已建立 CONNECTED）
                m_atClient.command("AT+QNETDEVSTATUS=1", to); // RmNet 链路状态 URC（方言层驱动消费）
                LogInfo <<"[RPi] URC framework enabled" ;

                // Check dual SIM support
                m_dualSimSupported = m_atClient.is_dual_sim_supported();
                m_dualSimChecked = true;
                if (m_dualSimSupported) {
                    LogInfo <<"[RPi] Dual SIM support detected" ;
                } else {
                    LogInfo <<"[RPi] Single SIM mode" ;
                }
            } else {
                LogInfo <<"[RPi] Warning: Failed to connect AT client to " << m_atPort ;
            }
        }

        LogInfo <<"[RPi] Module initialized" ;
        return InitStatus::SUCCESS;
    }

    bool RPIModuleInterface::deinit() {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized) {
            return false;
        }

        // Disconnect AT client
        if (m_atClient.is_connected()) {
            m_atClient.disconnect();
        }

        m_initialized = false;
        m_dualSimChecked = false;
        m_dualSimSupported = false;
        LogInfo <<"[RPi] Module deinitialized" ;
        return true;
    }

    void RPIModuleInterface::setAtPort(const std::string& port, int baud_rate) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_atPort = port;
        m_atBaudRate = baud_rate;

        // Reconnect if already initialized
        if (m_initialized && !m_atPort.empty()) {
            if (m_atClient.is_connected()) {
                m_atClient.disconnect();
            }
            if (m_atClient.connect(m_atPort, m_atBaudRate)) {
                LogInfo <<"[RPi] AT client connected to " << m_atPort << " at " << m_atBaudRate << " baud" ;

                // Re-check dual SIM support
                if (!m_dualSimChecked) {
                    m_dualSimSupported = m_atClient.is_dual_sim_supported();
                    m_dualSimChecked = true;
                }
            }
        }
    }

    // ============== Dual SIM Management ==============

    bool RPIModuleInterface::isDualSimSupported() {
        if (!m_dualSimChecked && m_atClient.is_connected()) {
            m_dualSimSupported = m_atClient.is_dual_sim_supported();
            m_dualSimChecked = true;
        }
        return m_dualSimSupported;
    }

    bool RPIModuleInterface::getDualSimInfo(DualSimInfo& dual_info) {
        if (!m_atClient.is_connected()) {
            return false;
        }

        auto status = m_atClient.get_dual_sim_status();
        if (!status) {
            return false;
        }

        dual_info.dual_sim_supported = status->dual_sim_supported;
        dual_info.active_slot = status->active_slot;

        for (int i = 0; i < 2; ++i) {
            dual_info.sim[i].sim_slot = i;
            dual_info.sim[i].imsi = status->sim[i].imsi;
            dual_info.sim[i].iccid = status->sim[i].iccid;
            dual_info.sim[i].present = status->sim[i].present;
            dual_info.sim[i].status = status->sim[i].ready ? SimStatus::READY :
                                      status->sim[i].present ? SimStatus::LOCKED : SimStatus::ABSENT;
        }

        return true;
    }

    bool RPIModuleInterface::selectSimSlot(int slot) {
        if (slot < 0 || slot > 1) {
            return false;
        }

        if (!m_atClient.is_connected()) {
            return false;
        }

        // For single SIM, just update internal state
        if (!isDualSimSupported()) {
            m_current_sim_id = slot;
            return (slot == 0);
        }

        return m_atClient.select_sim_slot(slot);
    }

    int RPIModuleInterface::getActiveSimSlot() {
        if (!m_atClient.is_connected()) {
            return 0;
        }

        if (!isDualSimSupported()) {
            return 0;
        }

        return m_atClient.get_active_sim_slot();
    }

    std::vector<NetworkSignalInfo> RPIModuleInterface::getAllSimSignalInfo() {
        std::vector<NetworkSignalInfo> results;

        if (!m_atClient.is_connected()) {
            return results;
        }

        if (!isDualSimSupported()) {
            // Single SIM mode
            NetworkSignalInfo info;
            if (getNetworkSignalInfo(0, info)) {
                results.push_back(info);
            }
            return results;
        }

        // Dual SIM mode - query each slot
        int original_slot = getActiveSimSlot();

        for (int slot = 0; slot < 2; ++slot) {
            NetworkSignalInfo info;
            if (getNetworkSignalInfo(slot, info)) {
                results.push_back(info);
            }
        }

        // Restore original slot
        if (original_slot >= 0) {
            selectSimSlot(original_slot);
        }

        return results;
    }

    // ============== Single SIM Methods ==============

    NetworkServiceStatus RPIModuleInterface::getNetworkServiceStatus(int sim_id) {
        // Use AT commands to check network service status
        if (!m_atClient.is_connected()) {
            return NetworkServiceStatus::UNKNOWN;
        }

        SimSlotGuard guard(m_atClient, sim_id, isDualSimSupported());
        if (!guard.success() && isDualSimSupported()) {
            return NetworkServiceStatus::UNKNOWN;
        }

        // Check LTE registration
        auto lte_reg = m_atClient.get_lte_registration();
        if (lte_reg && lte_reg->valid) {
            switch (lte_reg->stat) {
                case 1:  // Registered, home network
                case 5:  // Registered, roaming
                    return NetworkServiceStatus::NET_SERVICE_FULL;
                case 2:  // Searching
                    return NetworkServiceStatus::NET_SERVICE_NONE;
                case 3:  // Registration denied
                    return NetworkServiceStatus::NET_SERVICE_NONE;
                default:
                    break;
            }
        }

        // Check 5G registration
        auto nr_reg = m_atClient.get_5g_registration();
        if (nr_reg && nr_reg->valid) {
            switch (nr_reg->stat) {
                case 1:
                case 5:
                    return NetworkServiceStatus::NET_SERVICE_FULL;
                default:
                    break;
            }
        }

        return NetworkServiceStatus::UNKNOWN;
    }

    bool RPIModuleInterface::getNetworkSignalInfo(int sim_id, NetworkSignalInfo& signal_info) {
        signal_info.sim_slot = sim_id;

        if (!m_atClient.is_connected()) {
            signal_info.technology = "Unknown";
            signal_info.rsrp = 0;
            signal_info.rsrq = 0;
            signal_info.sinr = 0;
            return false;
        }

        // Switch to requested SIM slot (no-op for single SIM)
        SimSlotGuard guard(m_atClient, sim_id, isDualSimSupported());
        if (!guard.success() && isDualSimSupported()) {
            signal_info.technology = "Unknown";
            return false;
        }

        // Get 5G NR signal info
        auto nr_signal = m_atClient.get_nr_signal();
        if (nr_signal && nr_signal->valid) {
            signal_info.technology = "NR5G";
            signal_info.rsrp = nr_signal->rsrp;
            signal_info.rsrq = nr_signal->rsrq;
            signal_info.sinr = nr_signal->sinr;
            return true;
        }

        // Fallback: Get LTE signal if NR not available
        auto lte_signal = m_atClient.get_lte_signal();
        if (lte_signal && lte_signal->valid) {
            signal_info.technology = "LTE";
            signal_info.rsrp = lte_signal->rsrp;
            signal_info.rsrq = lte_signal->rsrq;
            signal_info.sinr = lte_signal->sinr;
            return true;
        }

        signal_info.technology = "Unknown";
        signal_info.rsrp = 0;
        signal_info.rsrq = 0;
        signal_info.sinr = 0;
        return false;
    }

    bool RPIModuleInterface::getNetworkCellInfo(int sim_id, NetworkCellInfo& cell_info) {
        cell_info.sim_slot = sim_id;

        if (!m_atClient.is_connected()) {
            cell_info.cell_id = "";
            cell_info.tac = "";
            cell_info.reg_stat = 0;
            cell_info.reg_act_type = 0;
            cell_info.rej_cause = 0;
            cell_info.rej_cause_type = 0;
            return false;
        }

        SimSlotGuard guard(m_atClient, sim_id, isDualSimSupported());
        if (!guard.success() && isDualSimSupported()) {
            return false;
        }

        // Get serving cell info
        auto cell = m_atClient.get_serving_cell();
        if (cell && cell->valid) {
            cell_info.cell_id = cell->cell_id;
            cell_info.tac = cell->tac;
            cell_info.reg_stat = 1;  // Registered
            return true;
        }

        cell_info.cell_id = "";
        cell_info.tac = "";
        cell_info.reg_stat = 0;
        cell_info.reg_act_type = 0;
        cell_info.rej_cause = 0;
        cell_info.rej_cause_type = 0;
        return false;
    }

    // 一次 AT 查询同时获取信号与小区信息，避免重复下发 AT+QENG="servingcell"
    RPIModuleInterface::NetworkWirelessInfo RPIModuleInterface::getNetworkWirelessInfo(int sim_id) {
        NetworkWirelessInfo wi;
        wi.signal.sim_slot = sim_id;
        wi.cell.sim_slot = sim_id;
        wi.signal.technology = "Unknown";

        if (!m_atClient.is_connected()) {
            return wi;
        }

        SimSlotGuard guard(m_atClient, sim_id, isDualSimSupported());
        if (!guard.success() && isDualSimSupported()) {
            return wi;
        }

        // 一次 AT+QENG="servingcell" 同时取 NR 信号与服务小区
        auto sc = m_atClient.get_servingcell_info();

        // 信号：NR 优先，LTE 回退
        if (sc.nr_signal && sc.nr_signal->valid) {
            wi.signal.technology = "NR5G";
            wi.signal.rsrp = sc.nr_signal->rsrp;
            wi.signal.rsrq = sc.nr_signal->rsrq;
            wi.signal.sinr = sc.nr_signal->sinr;
            wi.signal_valid = true;
        } else {
            auto lte_signal = m_atClient.get_lte_signal();
            if (lte_signal && lte_signal->valid) {
                wi.signal.technology = "LTE";
                wi.signal.rsrp = lte_signal->rsrp;
                wi.signal.rsrq = lte_signal->rsrq;
                wi.signal.sinr = lte_signal->sinr;
                wi.signal_valid = true;
            }
        }

        // 小区信息
        if (sc.cell && sc.cell->valid) {
            wi.cell.cell_id = sc.cell->cell_id;
            wi.cell.tac = sc.cell->tac;
            wi.cell.reg_stat = 1;  // Registered
            wi.cell_valid = true;
        }

        return wi;
    }

    bool RPIModuleInterface::getSimInfo(int sim_id, SimInfo& sim_info) {
        sim_info.sim_slot = sim_id;

        if (!m_atClient.is_connected()) {
            sim_info.imsi = "";
            sim_info.iccid = "";
            sim_info.status = SimStatus::ABSENT;
            sim_info.present = false;
            return false;
        }

        // For dual SIM, switch to the requested slot
        if (isDualSimSupported()) {
            auto status = m_atClient.get_sim_status_slot(sim_id);
            if (status) {
                sim_info.sim_slot = status->sim_slot;
                sim_info.imsi = status->imsi;
                sim_info.iccid = status->iccid;
                sim_info.present = status->present;
                sim_info.status = status->ready ? SimStatus::READY :
                                  status->present ? SimStatus::LOCKED : SimStatus::ABSENT;
                return true;
            }
        } else {
            // Single SIM mode：IMSI/ICCID 静态值缓存复用，仅每周期查 CPIN? 取状态
            bool idsCached = m_simIdsCached;
            std::string cachedImsi, cachedIccid;
            if (idsCached) {
                std::lock_guard<std::mutex> lk(m_mutex);
                cachedImsi = m_cachedImsi;
                cachedIccid = m_cachedIccid;
            }
            auto status = m_atClient.get_sim_status(!idsCached);
            if (status) {
                if (idsCached) {
                    sim_info.imsi = cachedImsi;
                    sim_info.iccid = cachedIccid;
                } else {
                    sim_info.imsi = status->imsi;
                    sim_info.iccid = status->iccid;
                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_cachedImsi = status->imsi;
                    m_cachedIccid = status->iccid;
                    m_simIdsCached = true;
                }
                sim_info.present = status->ready || !sim_info.iccid.empty();
                sim_info.status = status->ready ? SimStatus::READY : SimStatus::LOCKED;
                return true;
            }
        }

        sim_info.imsi = "";
        sim_info.iccid = "";
        sim_info.status = SimStatus::ABSENT;
        sim_info.present = false;
        return false;
    }

    bool RPIModuleInterface::getDeviceInfo(DeviceInfo& devinfo) {
        // 缓存命中：设备信息(厂商/型号/固件/IMEI/SN)运行期不变，直接返回
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_devInfoCached) {
                devinfo = m_cachedDeviceInfo;
                return true;
            }
        }
        // Try to get module info from AT commands first
        if (m_atClient.is_connected()) {
            auto module_info = m_atClient.get_module_info();
            if (module_info && module_info->valid) {
                devinfo.imei = module_info->imei;
                devinfo.modem_vendor = module_info->manufacturer;
                devinfo.modem_model = module_info->model;
                devinfo.firmware_version = module_info->revision;
                devinfo.sn = module_info->sn;
                std::lock_guard<std::mutex> lk(m_mutex);
                m_cachedDeviceInfo = devinfo;
                m_devInfoCached = true;
                return true;
            }
        }

        // Fallback: get basic system info for RPi
        struct utsname buf;
        if (uname(&buf) == 0) {
            devinfo.modem_model = std::string(buf.machine);
            devinfo.hardware_version = std::string(buf.release);
            devinfo.software_version = std::string(buf.version);
        }

        // Get hostname as serial number fallback
        char hostname[256] = {0};
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            devinfo.sn = std::string(hostname);
        }

        // Read CPU info for Raspberry Pi
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.find("Serial") != std::string::npos) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    devinfo.sn = line.substr(pos + 2);
                }
            }
            if (line.find("Model") != std::string::npos) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    devinfo.modem_model = line.substr(pos + 2);
                }
            }
            if (line.find("Revision") != std::string::npos) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    devinfo.hardware_version = line.substr(pos + 2);
                }
            }
        }

        return true;
    }

    std::string RPIModuleInterface::getImei(int sim_id) {
        // Use AT commands to get IMEI
        if (m_atClient.is_connected()) {
            auto imei = m_atClient.get_imei();
            if (imei) {
                return *imei;
            }
        }
        return "";
    }

    std::string RPIModuleInterface::getSerialNumber() {
        // Read serial from /proc/cpuinfo
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.find("Serial") != std::string::npos) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    return line.substr(pos + 2);
                }
            }
        }
        return "rpi-unknown";
    }

    std::string RPIModuleInterface::getFirmwareVersion() {
        struct utsname buf;
        if (uname(&buf) == 0) {
            return std::string(buf.release);
        }
        return "unknown";
    }

    int RPIModuleInterface::getCurrentDataCard() {
        return getActiveSimSlot();
    }

    bool RPIModuleInterface::getAPN(int sim_id, int cid, std::string& apn) {
        apn = "";
        return false;
    }

    // ============== System Info ==============

    float RPIModuleInterface::getMemoryUsage() {
        std::ifstream file("/proc/meminfo");
        std::string line;
        unsigned long long totalMemory = 0;
        unsigned long long freeMemory = 0;
        unsigned long long availableMemory = 0;

        while (std::getline(file, line)) {
            if (line.find("MemTotal") != std::string::npos) {
                std::istringstream iss(line);
                std::string label;
                iss >> label >> totalMemory;
            } else if (line.find("MemAvailable") != std::string::npos) {
                std::istringstream iss(line);
                std::string label;
                iss >> label >> availableMemory;
            } else if (line.find("MemFree") != std::string::npos) {
                std::istringstream iss(line);
                std::string label;
                iss >> label >> freeMemory;
            }
        }

        unsigned long long usedMemory = totalMemory - (availableMemory > 0 ? availableMemory : freeMemory);
        return (static_cast<float>(usedMemory) / static_cast<float>(totalMemory)) * 100.0f;
    }

    std::vector<unsigned long long> RPIModuleInterface::getCpuTime() {
        std::ifstream file("/proc/stat");
        std::string line;
        std::getline(file, line);

        std::istringstream iss(line);
        std::string cpu;
        unsigned long long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
        iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;

        return {user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice};
    }

    float RPIModuleInterface::getCpuUsage() {
        static std::vector<unsigned long long> lastCpuTime;
        std::vector<unsigned long long> currentCpuTime = getCpuTime();

        if (lastCpuTime.empty()) {
            lastCpuTime = currentCpuTime;
            return 0.0f;
        }

        unsigned long long totalTimeNow = 0;
        unsigned long long totalTimeLast = 0;
        unsigned long long idleTimeNow = 0;
        unsigned long long idleTimeLast = 0;

        for (size_t i = 0; i < currentCpuTime.size(); ++i) {
            totalTimeNow += currentCpuTime[i];
            totalTimeLast += lastCpuTime[i];
        }

        idleTimeNow = currentCpuTime[3] + currentCpuTime[4];
        idleTimeLast = lastCpuTime[3] + lastCpuTime[4];

        unsigned long long totalDelta = totalTimeNow - totalTimeLast;
        unsigned long long idleDelta = idleTimeNow - idleTimeLast;

        lastCpuTime = currentCpuTime;

        if (totalDelta == 0) return 0.0f;
        return (1.0f - static_cast<float>(idleDelta) / static_cast<float>(totalDelta)) * 100.0f;
    }

    // ============== GNSS (RM520N-GL) ==============

    bool RPIModuleInterface::enableGnss(int mode) {
        if (!m_atClient.is_connected()) return false;
        // AT+QGPS=<mode>；若 GNSS 已开启，模组返回 ERROR，此处 false 由调用方按"已开"处理
        return m_atClient.gps_enable(mode);
    }

    std::optional<tbox::GpsLocation> RPIModuleInterface::getGpsLocation() {
        if (!m_atClient.is_connected()) return std::nullopt;
        // 先 AT+CGPSINFO，回退 AT+QGPSLOC=2；无定位时返回 nullopt
        return m_atClient.get_location();
    }

}  // namespace rpi