/**********************************************************************************************************************
    > File Name: rpi_module_interface.cpp
    > Author: Generated for Raspberry Pi
    > Date: 03/06/26
    > Description: Implementation for Raspberry Pi module interface with dual SIM support
**********************************************************************************************************************/
#include "rpi_module_interface.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <sys/utsname.h>

namespace rpi {

    // SimSlotGuard implementation - switches to target slot and restores on destruction
    RPIModuleInterface::SimSlotGuard::SimSlotGuard(tbox::AtClient& client, int target_slot)
        : client_(client), original_slot_(-1), success_(false)
    {
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
                std::cout << "[RPi] AT client connected to " << m_atPort << " at " << m_atBaudRate << " baud" << std::endl;

                // ---- URC setup ----
                // Register default URC handlers. Note: +CEREG/+CREG are intentionally
                // NOT subscribed here because their URC lines share the response
                // prefix with AT+CEREG?/AT+CREG? and cannot be distinguished by
                // prefix alone. They keep using polling.
                m_atClient.register_urc_handler("RDY", [](const std::string& l) {
                    std::cout << "[RPi] URC module ready: " << l << std::endl;
                });
                m_atClient.register_urc_handler("+CFUN:", [](const std::string& l) {
                    std::cout << "[RPi] URC radio function: " << l << std::endl;
                });
                m_atClient.register_urc_handler("+QSIMSTAT:", [](const std::string& l) {
                    std::cout << "[RPi] URC SIM status: " << l << std::endl;
                });
                m_atClient.register_urc_handler("+CMTI:", [](const std::string& l) {
                    std::cout << "[RPi] URC SMS received: " << l << std::endl;
                });

                // Enable selected unsolicited reports (ignore failures: not all
                // modules support every command).
                const auto to = std::chrono::milliseconds(1500);
                m_atClient.command("AT+QSIMSTAT=1", to);   // SIM hot-plug URC
                m_atClient.command("AT+CNMI=2,1,0,0,0", to); // SMS arrival URC
                std::cout << "[RPi] URC framework enabled" << std::endl;

                // Check dual SIM support
                m_dualSimSupported = m_atClient.is_dual_sim_supported();
                m_dualSimChecked = true;
                if (m_dualSimSupported) {
                    std::cout << "[RPi] Dual SIM support detected" << std::endl;
                } else {
                    std::cout << "[RPi] Single SIM mode" << std::endl;
                }
            } else {
                std::cout << "[RPi] Warning: Failed to connect AT client to " << m_atPort << std::endl;
            }
        }

        std::cout << "[RPi] Module initialized" << std::endl;
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
        std::cout << "[RPi] Module deinitialized" << std::endl;
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
                std::cout << "[RPi] AT client connected to " << m_atPort << " at " << m_atBaudRate << " baud" << std::endl;

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

        SimSlotGuard guard(m_atClient, sim_id);
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
        SimSlotGuard guard(m_atClient, sim_id);
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

        SimSlotGuard guard(m_atClient, sim_id);
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

        SimSlotGuard guard(m_atClient, sim_id);
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
            // Single SIM mode
            auto status = m_atClient.get_sim_status();
            if (status) {
                sim_info.imsi = status->imsi;
                sim_info.iccid = status->iccid;
                sim_info.present = status->ready || !status->iccid.empty();
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
        // Try to get module info from AT commands first
        if (m_atClient.is_connected()) {
            auto module_info = m_atClient.get_module_info();
            if (module_info && module_info->valid) {
                devinfo.imei = module_info->imei;
                devinfo.modem_vendor = module_info->manufacturer;
                devinfo.modem_model = module_info->model;
                devinfo.firmware_version = module_info->revision;
                devinfo.sn = module_info->sn;
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

}  // namespace rpi