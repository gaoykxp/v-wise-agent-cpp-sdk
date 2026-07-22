/**********************************************************************************************************************
    > File Name: rpi_module_interface.h
    > Author: Generated for cross-platform support
    > Date: 03/06/26
    > Description: Raspberry Pi module interface for cellular modem communication
**********************************************************************************************************************/

#ifndef RPI_MODULE_INTERFACE_H
#define RPI_MODULE_INTERFACE_H

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include "rpi_at_client.h"

namespace rpi {

    // Enum definitions
    enum class InitStatus {
        SUCCESS,
        FAILURE,
        ALREADY_INITIALIZED
    };

    enum class NetworkServiceStatus {
        UNKNOWN,
        NET_SERVICE_NONE,
        NET_SERVICE_LIMITED,
        NET_SERVICE_FULL
    };

    enum class SimStatus {
        ABSENT,
        LOCKED,
        INITIALIZING,
        READY
    };

    enum class NetworkRegStatus {
        NOT_REGISTERED,
        REGISTERED_HOME,
        SEARCHING,
        REGISTRATION_DENIED,
        REGISTERED_ROAMING,
        LIMMITED
    };

    // Struct definitions
    struct NetworkSignalInfo {
        int sim_slot = 0;           // SIM card slot (0 or 1)
        std::string technology;
        int rsrp = 0;
        int rsrq = 0;
        int sinr = 0;
    };

    struct NetworkCellInfo {
        int sim_slot = 0;           // SIM card slot (0 or 1)
        std::string cell_id;
        std::string tac;
        int reg_stat = 0;
        int reg_act_type = 0;
        int rej_cause = 0;
        int rej_cause_type = 0;
    };

    struct SimInfo {
        int sim_slot = 0;           // SIM card slot (0 or 1)
        std::string imsi;
        std::string iccid;
        SimStatus status = SimStatus::ABSENT;
        bool present = false;       // SIM card is inserted
    };

    // Dual SIM information
    struct DualSimInfo {
        SimInfo sim[2];             // sim[0] for slot 0, sim[1] for slot 1
        int active_slot = 0;        // Currently active SIM slot
        bool dual_sim_supported = false;
    };

    struct DeviceInfo {
        std::string imei;
        std::string sn;
        std::string firmware_version;
        std::string software_version;
        std::string hardware_version;
        std::string modem_vendor;
        std::string modem_model;
    };

    // RPIModuleInterface class
    class RPIModuleInterface {
    public:
        static RPIModuleInterface& getInstance();

        InitStatus init(int sim_id = 0);
        bool deinit();

        // Set AT command port (e.g., "/dev/ttyUSB2")
        void setAtPort(const std::string& port, int baud_rate = 115200);

        // ============== Single SIM Methods (sim_id: 0 or 1) ==============
        NetworkServiceStatus getNetworkServiceStatus(int sim_id);
        bool getNetworkSignalInfo(int sim_id, NetworkSignalInfo& signal_info);
        bool getNetworkCellInfo(int sim_id, NetworkCellInfo& cell_info);

        // 一次 AT 查询同时获取信号与小区信息，避免重复下发 AT+QENG="servingcell"
        struct NetworkWirelessInfo {
            NetworkSignalInfo signal;
            NetworkCellInfo cell;
            bool signal_valid{false};
            bool cell_valid{false};
        };
        NetworkWirelessInfo getNetworkWirelessInfo(int sim_id);
        bool getSimInfo(int sim_id, SimInfo& sim_info);
        bool getDeviceInfo(DeviceInfo& devinfo);

        std::string getImei(int sim_id = 0);
        std::string getSerialNumber();
        std::string getFirmwareVersion();
        int getCurrentDataCard();
        bool getAPN(int sim_id, int cid, std::string& apn);

        // ============== Dual SIM Management ==============
        // Check if dual SIM is supported
        bool isDualSimSupported();

        // Get dual SIM information (both slots at once)
        bool getDualSimInfo(DualSimInfo& dual_info);

        // Select active SIM slot for data/connection (0 or 1)
        bool selectSimSlot(int slot);

        // Get currently active SIM slot (returns 0 or 1, -1 on error)
        int getActiveSimSlot();

        // Get all SIM signal info (returns map<sim_slot, signal_info>)
        std::vector<NetworkSignalInfo> getAllSimSignalInfo();

        // ============== System Info ==============
        float getMemoryUsage();
        std::vector<unsigned long long> getCpuTime();
        float getCpuUsage();

    private:
        RPIModuleInterface() = default;
        ~RPIModuleInterface() = default;
        RPIModuleInterface(const RPIModuleInterface&) = delete;
        RPIModuleInterface& operator=(const RPIModuleInterface&) = delete;

        // Helper to switch SIM slot and restore
        class SimSlotGuard {
        public:
            SimSlotGuard(tbox::AtClient& client, int target_slot);
            ~SimSlotGuard();
            bool success() const { return success_; }
        private:
            tbox::AtClient& client_;
            int original_slot_;
            bool success_;
        };

        std::mutex m_mutex;
        bool m_initialized = false;
        int m_current_sim_id = 0;
        tbox::AtClient m_atClient;
        std::string m_atPort;
        int m_atBaudRate = 115200;
        bool m_dualSimSupported = false;
        bool m_dualSimChecked = false;
    };

}  // namespace rpi

#endif  // RPI_MODULE_INTERFACE_H