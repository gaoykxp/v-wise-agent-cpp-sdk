/**********************************************************************************************************************
    > File Name: probe_mgr.cpp
    > Author: hrliu dmhuang
    > Date: 12/20/23
**********************************************************************************************************************/

#include "probe_mgr.h"
#include "base_timer.h"
#include "rpi_gps_info.h"
#include "global.h"
#include "log.h"
#ifdef ENABLE_MINIO
#include "minio.h"
#endif
#include "rpi_module_interface.h"
#include <cstdio>
#include <functional>
#include <iostream>
#include <random>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

namespace {
    using namespace cmsr::vwise;
    using json = nlohmann::json;
#define FAULT_FILE_SIM "faultSim"
#define FAULT_FILE_NET "faultNet"

    // 无线错误码集合，按需随机选取一条用于平台异常监测展示
    std::string pickWirelessErrorCode() {
        static const std::vector<std::string> kErrorCodes = {
            "WEAK_SIGNAL", "NET_RESOURCE_SHORT", "NO_NET_RESOURCE",
            "G4G5_SWITCH", "REBOOT", "BANDWIDTH_CONFLICT",
            "REGISTER_FAIL", "SWITCH_FAIL"
        };
        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<std::size_t> dist(0, kErrorCodes.size() - 1);
        return kErrorCodes[dist(gen)];
    }

    bool isFileExists(const std::string &filename) {
        std::ifstream file(filename);
        return file.good();
    }
    bool readFileLine(const std::string &filename, std::string &content) {
        std::ifstream file(filename, std::ios::in);
        if (!file.is_open()) {
            std::cerr << "Error: Unable to open file '" << filename << "' for reading." << std::endl;
            return false;
        }

        std::getline(file, content);

        return true;
    }

    void saveFault(const std::string &fileName, const std::string &fault) {
        json fault_data;
        try {
            // 解析fault字符串
            if (!fault.empty()) {
                fault_data = json::parse(fault);
            }
        } catch (const json::parse_error &e) {
            LogError << "Failed to parse existing fault data: " << e.what();
            return;
        }

        if (!isFileExists(fileName)) {//第一次产生异常，记录时间，并写入file文件
            fault_data["first_ts"] = base_tools::BaseTimer::GetMilliTime();
        } else {//已经有异常日志，读取文件内容，获取第一次时间，并插入到fault后保存
            std::string file_data;
            if (!readFileLine(fileName, file_data)) {
                LogError << "Failed to read existing fault data from file.";
                return;
            }
            // 解析file_data
            if (!file_data.empty()) {
                try {
                    json file_data_json = json::parse(file_data);
                    fault_data["first_ts"] = file_data_json["first_ts"];
                } catch (const json::parse_error &e) {
                    LogError << "Failed to parse existing fault data: " << e.what();
                    fault_data["first_ts"] = base_tools::BaseTimer::GetMilliTime();
                }
            }
        }
        // 将更新后的数据写入文件
        std::ofstream file(fileName, std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
            LogError << "Failed to open file '" << fileName << "' for writing.";
            return;
        }
        file << fault_data.dump() << std::endl;
        file.flush();
    }
    void clearFault() {
        if (!isFileExists(FAULT_FILE_SIM)) {
            LogDebug << "no sim fault log.";
        } else {
            std::remove(FAULT_FILE_SIM);// 删除fault文件
            LogDebug << "sim fault log cleared.";
        }

        if (!isFileExists(FAULT_FILE_NET)) {
            LogDebug << "no net fault log.";
        } else {
            std::remove(FAULT_FILE_NET);// 删除fault文件
            LogDebug << "net fault log cleared.";
        }
        return;
    }

    void pingSendMsg(std::string taskId, std::string targetIp) {
        LogInfo << "pingSendMsg start.";
        nlohmann::json j;
        j["task_id"] = taskId;
        j["target_ip"] = targetIp;
        j["time"] = base_tools::util::sysTimeMqtt();
        j["lose_rate"] = PING::getInstance().m_loss.load();//.load(std::memory_order_relaxed);
        j["delay"] = PING::getInstance().m_pingTime.load();//.load(std::memory_order_relaxed);
        GPSData gpsData = GPS::getInstance().gData;
        j["latitude"] = gpsData.latitude;
        j["longitude"] = gpsData.longitude;
        j["altitude"] = gpsData.altitude;
        std::string strJ = j.dump();
        std::string fileName = "/mnt/data/log/ProbeLog/" + taskId + ".txt";
        base_tools::util::writeLogToFile(fileName, strJ);
        ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_DVICE_PING_UP, strJ);
    }

    void wirelessSendMsg(std::string taskId) {
        LogInfo << "wirelessSendMsg start.";

        auto message = CellInfoCollector::getInstance().GetMStrJ();
        if (!message.empty()) {
            auto j = json::parse(message);
            GPSData gpsData = GPS::getInstance().gData;
            j["task_id"] = taskId;
            j["latitude"] = gpsData.latitude;
            j["longitude"] = gpsData.longitude;
            j["altitude"] = gpsData.altitude;
            std::string msg = j.dump();
            std::string fileName = "/mnt/data/log/ProbeLog/" + taskId + ".txt";
            base_tools::util::writeLogToFile(fileName, msg);
            ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_DVICE_WIRELESS_UP, msg);
        }
    }

    void pc5SendMsg(std::string taskId) {
        // PC5 functionality removed
        LogWarn << "pc5SendMsg: PC5 functionality has been removed";
    }

    void bsmSendMsg(std::string taskId) {
        GPSData gpsData = GPS::getInstance().gData;
        json j;
        j["task_id"] = taskId;
        j["bsm"]["latitude"] = gpsData.latitude;
        j["bsm"]["longitude"] = gpsData.longitude;
        j["bsm"]["altitude"] = gpsData.altitude;
        j["bsm"]["speed"] = gpsData.speed;
        j["bsm"]["course"] = gpsData.course;
        j["time"] = base_tools::BaseTimer::GetMilliTime();
        std::string strJ = j.dump();
        std::string fileName = "/mnt/data/log/ProbeLog/" + taskId + ".txt";
        base_tools::util::writeLogToFile(fileName, strJ);
        // BSM topic removed - only log to file
    }

    void uuReportSendMsg() {
        auto message = CellInfoCollector::getInstance().GetMStrJ();
        if (!message.empty()) {
            auto j = json::parse(message);
            nlohmann::json j1;
            j1["rsrp"] = j["cell_info"][0]["rsrp"];
            j1["sinr"] = j["cell_info"][0]["sinr"];
            j1["rat"] = j["cell_info"][0]["rat"];

            GPSData gpsData = GPS::getInstance().gData;
            j1["latitude"] = gpsData.latitude;
            j1["longitude"] = gpsData.longitude;
            j1["altitude"] = gpsData.altitude;

            std::string msg = j1.dump();
            ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_DVICE_WIRELESS_UP, msg);
        }
    }

    void userOnlineSendMsg() {
        nlohmann::json j;
        j["imei"] = ProbeMgr::getInstance().m_imei;
        std::string strJ = j.dump();
        ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_USER_ONLINE, strJ);
    }

    void HeartBeatSendMsg() {

        nlohmann::json j;
        json inner_list = json::array();
        {
            std::lock_guard<std::recursive_mutex> lock(ProbeMgr::getInstance().mtx);

            for (int i = 0; i < ProbeMgr::getInstance().vTaskMgr.size(); i++) {
                nlohmann::json j1;
                j1["task_id"] = ProbeMgr::getInstance().vTaskMgr[i].taskId;
                j1["status"] = ProbeMgr::getInstance().vTaskMgr[i].taskControl;
                inner_list.push_back(j1);
            }
        }
        j["task_list"] = inner_list;
        std::string strJ = j.dump();

        ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_USER_HEARTBEAT, strJ);
    }
}// namespace

namespace cmsr {
    namespace vwise {
        using namespace std;
        using namespace std::placeholders;
        using json = nlohmann::json;

        ProbeMgr &ProbeMgr::getInstance() {
            static ProbeMgr m_instance;
            return m_instance;
        }

        ProbeMgr::ProbeMgr() : m_imei{std::move(common::GlobalData::Instance()->getImei())} {
        }

        ProbeMgr::~ProbeMgr() {}

        void ProbeMgr::probeTaskDelete(std::string taskId) {
            for (int index = 0; index < ProbeMgr::getInstance().vTaskMgr.size(); index++) {
                if (vTaskMgr[index].taskId == taskId) {
                    vTaskMgr.erase(ProbeMgr::getInstance().vTaskMgr.begin() + index);
                    break;
                }
            }
        }

        void ProbeMgr::vwiseProbeTaskHandle() {
            json jpb;
            // 获取设备信息
            auto &module = rpi::RPIModuleInterface::getInstance();

            // cpu 利用率
            auto cpuUsage = module.getCpuUsage();
            if (cpuUsage <= 0.0) {
                return;
            }
            // std::cout << "CPU Usage: " << cpuUsage << "%" << std::endl;
            if (m_collectCategory == CollectFull) {
                jpb["system"]["cpu_util"] = static_cast<int>(cpuUsage);
            }

            if (m_collectCategory == CollectFull) {
                rpi::DeviceInfo devInfo;
                if (module.getDeviceInfo(devInfo)) {
                    // 按照zd_wrapper.h里面定义的json格式填充
                    jpb["system"]["imei"] = devInfo.imei;
                    jpb["system"]["sn"] = devInfo.sn;
                    jpb["system"]["sw_version"] = devInfo.software_version;
                    jpb["system"]["hw_version"] = devInfo.hardware_version;
                }
                jpb["system"]["mem_util"] = static_cast<int>(module.getMemoryUsage());
            }
            auto timestamp = base_tools::BaseTimer::GetMilliTime();
            jpb["system"]["timestamp"] = timestamp;

            if (m_collectCategory == CollectFull) {
                if (PING::getInstance().isPingEnabled()) {
                    jpb["system"]["net_delay"] = PING::getInstance().m_pingTime.load();
                    jpb["system"]["net_loss"] = PING::getInstance().m_loss.load();
                }

                GPSData gpsData = GPS::getInstance().gData;
                jpb["system"]["longitude"] = gpsData.longitude;
                jpb["system"]["latitude"] = gpsData.latitude;
            }

            // 获取SIM信息（核心模式下仍需采集用于故障判定，但不写入上报payload）
            rpi::SimInfo simInfo;
            {
                module.getSimInfo(0, simInfo);
                // std::cout << "SIM IMSI: " << simInfo.imsi << std::endl;
                // std::cout << "SIM Status: " << static_cast<int>(simInfo.status) << std::endl;
                // std::cout << "SIM iccid: " << simInfo.iccid << std::endl;
                if (m_collectCategory == CollectFull) {
                    if (!simInfo.imsi.empty()) {
                        jpb["sim_card"]["imsi"] = simInfo.imsi;
                    }
                    if (!simInfo.iccid.empty()) {
                        jpb["sim_card"]["iccid"] = simInfo.iccid;
                    }

                    switch (simInfo.status) {
                        case rpi::SimStatus::ABSENT:
                            jpb["sim_card"]["status"] = "ABSENT";
                            break;
                        case rpi::SimStatus::LOCKED:
                            jpb["sim_card"]["status"] = "LOCKED";
                            break;
                        case rpi::SimStatus::INITIALIZING:
                            jpb["sim_card"]["status"] = "INITIALIZING";
                            break;
                        case rpi::SimStatus::READY:
                            jpb["sim_card"]["status"] = "READY";
                            break;
                        default:
                            jpb["sim_card"]["status"] = "UNKNOWN";
                            break;
                    }
                }
            }

            // 一次 AT 查询同时获取信号与小区信息（避免重复 AT+QENG="servingcell"）
            auto wirelessInfo = module.getNetworkWirelessInfo(0);
            const auto &signalInfo = wirelessInfo.signal;
            const auto &cell_info = wirelessInfo.cell;
            if (wirelessInfo.signal_valid) {
                jpb["wireless"]["net_type"] = signalInfo.technology;
                jpb["wireless"]["rsrp"] = signalInfo.rsrp;
                jpb["wireless"]["rsrq"] = signalInfo.rsrq;
                jpb["wireless"]["sinr"] = signalInfo.sinr;
            }
            jpb["wireless"]["reg_stat"] = "UNKNOWN";
            if (wirelessInfo.cell_valid) {
                // std::cout << "Return Action Type: " << cell_info.reg_act_type << std::endl;
                jpb["wireless"]["cid"] = cell_info.cell_id;
                jpb["wireless"]["tac"] = cell_info.tac;
                jpb["wireless"]["reg_err_code"] = cell_info.rej_cause;

                switch (cell_info.reg_stat) {
                    case static_cast<int>(rpi::NetworkRegStatus::NOT_REGISTERED):
                        jpb["wireless"]["reg_stat"] = "NOT_REGISTERED";
                        break;
                    case static_cast<int>(rpi::NetworkRegStatus::REGISTERED_HOME):
                        jpb["wireless"]["reg_stat"] = "REGISTERED_HOME";
                        break;
                    case static_cast<int>(rpi::NetworkRegStatus::SEARCHING):
                        jpb["wireless"]["reg_stat"] = "SEARCHING";
                        break;
                    case static_cast<int>(rpi::NetworkRegStatus::REGISTRATION_DENIED):
                        jpb["wireless"]["reg_stat"] = "REGISTRATION_DENIED";
                        break;
                    case static_cast<int>(rpi::NetworkRegStatus::REGISTERED_ROAMING):
                        jpb["wireless"]["reg_stat"] = "REGISTERED_ROAMING";
                        break;
                    case static_cast<int>(rpi::NetworkRegStatus::LIMMITED):
                        jpb["wireless"]["reg_stat"] = "LIMMITED";
                        break;
                    default:
                        jpb["wireless"]["reg_stat"] = "UNKNOWN";
                        break;
                }
            }

            // 错误码：从预设集合随机选取，随 reg_stat 之后写入 wireless
            jpb["wireless"]["error_code"] = pickWirelessErrorCode();

            std::string apn;
            module.getAPN(0, 1, apn);
            // std::cout << "APN1: " << apn << std::endl;
            if (m_collectCategory == CollectFull) {
                jpb["wireless"]["apn"] = apn;
            }

            auto zdpb_str = jpb.dump();
            LogDebug << "jpb: " << zdpb_str;
            //fault handle
            // LogDebug << "cell_info.reg_stat: " << cell_info.reg_stat;
            if (simInfo.status == rpi::SimStatus::ABSENT || simInfo.status == rpi::SimStatus::LOCKED) {
                //保存故障信息
                saveFault(FAULT_FILE_SIM, zdpb_str);
                LogInfo << "ProbeMgrP::saveSimFault\n";

            } else if (cell_info.cell_id.empty() ||
                       cell_info.reg_stat != static_cast<int>(rpi::NetworkRegStatus::REGISTERED_HOME) ||
                       ((signalInfo.technology == "LTE") && (signalInfo.rsrp <= m_noNetLTEThr)) ||
                       ((signalInfo.technology == "NR") && (signalInfo.rsrp <= m_noNet5GThr))) {
                //保存故障信息
                saveFault(FAULT_FILE_NET, zdpb_str);
                LogInfo << "ProbeMgrP::saveNetFault\n";
            } //else {//正常
            //just for test MQTT: GYK
            if(1){
                //如果fault存在，读取fault，并发送fault
                std::string fault_str;

                if (isFileExists(FAULT_FILE_SIM) && readFileLine(FAULT_FILE_SIM, fault_str)) {
                    ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_DVICE_VW_FAULT_UP, fault_str);
                }
                if (isFileExists(FAULT_FILE_NET) && readFileLine(FAULT_FILE_NET, fault_str)) {
                    ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_DVICE_VW_FAULT_UP, fault_str);
                }
                // 正常发送数据
                ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_DVICE_VW_DATA_UP, zdpb_str);
                LogInfo << "ProbeMgrP::mqtt.messageSend topic:" << TOPIC_DVICE_VW_DATA_UP;
            }
        }

        int ProbeMgr::probeTaskSearch(std::string taskId) {
            int index;
            for (index = 0; index < ProbeMgr::getInstance().vTaskMgr.size(); index++) {
                if (vTaskMgr[index].taskId == taskId) {
                    break;
                }
            }
            ProbeMgr::getInstance().vTaskMgr[index].execNum--;
            return ProbeMgr::getInstance().vTaskMgr[index].execNum;
        }

        void ProbeMgr::readJsonFile(const std::string &cfgPath) {
            std::string TopicPrefix;
            std::string UserOnline;
            // std::fstream fin(cfgPath);
            // if (nlohmann::json::accept(fin))
            {
                // nlohmann::json j = nlohmann::json::parse(fin);
                nlohmann::json jrw = common::GlobalData::Instance()->getRwJson();

                if (jrw.is_discarded()) {
                    LogInfo << "readJsonFile is error";
                    return;
                } else {
                    if (jrw.is_object()) {
                        if (jrw["ProbeMqtt"].is_object()) {
                            nlohmann::json jMqtt = jrw["ProbeMqtt"];
                            if (jMqtt["Address"].is_string())
                                jMqtt.at("Address").get_to(broker.address);
                            if (jMqtt["UserName"].is_string())
                                 jMqtt.at("UserName").get_to(broker.username);
                            if (jMqtt["Password"].is_string())
                                 jMqtt.at("Password").get_to(broker.password);
                            if (jMqtt["ClientId"].is_string())
                                jMqtt.at("ClientId").get_to(broker.clientId);
                            // sub
                            if (jMqtt["OrderDown"].is_string()) {
                                jMqtt.at("OrderDown").get_to(TOPIC_PLAT_ORDER_DOWN);
                                TOPIC_PLAT_ORDER_DOWN.insert(14, m_imei);
                            }
                            if (jMqtt["OrderDownAck"].is_string()) {
                                jMqtt.at("OrderDownAck").get_to(TOPIC_PLAT_ORDER_DOWN_ACK);
                                TOPIC_PLAT_ORDER_DOWN_ACK.insert(14, m_imei);
                            }
                            if (jMqtt["UploadDown"].is_string()) {
                                jMqtt.at("UploadDown").get_to(TOPIC_PLAT_UPLOAD_DOWN);
                                TOPIC_PLAT_UPLOAD_DOWN.insert(14, m_imei);
                            }

                            if (jMqtt["UserOnlineAck"].is_string()) {
                                jMqtt.at("UserOnlineAck").get_to(TOPIC_USER_ONLINE_ACK);
                                TOPIC_USER_ONLINE_ACK.insert(14, m_imei);
                            }

                            if (jMqtt["SwitchChannels"].is_string()) {
                                jMqtt.at("SwitchChannels").get_to(TOPIC_USER_SWITCHING_CHANNELS);
                                TOPIC_USER_SWITCHING_CHANNELS.insert(14, m_imei);
                            }

                            //  /switching/channels，少了
                            // pub
                            if (jMqtt["UserOnline"].is_string()) {
                                jMqtt.at("UserOnline").get_to(TOPIC_USER_ONLINE);
                                TOPIC_USER_ONLINE.insert(14, m_imei);
                            }

                            if (jMqtt["Heartbeat"].is_string()) {
                                jMqtt.at("Heartbeat").get_to(TOPIC_USER_HEARTBEAT);
                                TOPIC_USER_HEARTBEAT.insert(14, m_imei);
                            }

                            if (jMqtt["PingUp"].is_string()) {
                                jMqtt.at("PingUp").get_to(TOPIC_DVICE_PING_UP);
                                TOPIC_DVICE_PING_UP.insert(14, m_imei);
                            }

                            if (jMqtt["WirelessUp"].is_string()) {
                                jMqtt.at("WirelessUp").get_to(TOPIC_DVICE_WIRELESS_UP);
                                TOPIC_DVICE_WIRELESS_UP.insert(14, m_imei);
                            }

                            if (jMqtt["UploadDownAck"].is_string()) {
                                jMqtt.at("UploadDownAck").get_to(TOPIC_PLAT_UPLOAD_DOWN_ACK);
                                TOPIC_PLAT_UPLOAD_DOWN_ACK.insert(14, m_imei);
                            }

                            if (jMqtt["SwitchChannelsAck"].is_string()) {
                                jMqtt.at("SwitchChannelsAck").get_to(TOPIC_USER_SWITCHING_CHANNELS_ACK);
                                TOPIC_USER_SWITCHING_CHANNELS_ACK.insert(14, m_imei);
                            }

                            if (jMqtt["DataReport"].is_string()) {
                                jMqtt.at("DataReport").get_to(TOPIC_DVICE_VW_DATA_UP);
                                TOPIC_DVICE_VW_DATA_UP.insert(14, m_imei);
                            }
                            if (jMqtt["FaultReport"].is_string()) {
                                jMqtt.at("FaultReport").get_to(TOPIC_DVICE_VW_FAULT_UP);
                                TOPIC_DVICE_VW_FAULT_UP.insert(14, m_imei);
                            }
                            if (jMqtt["FaultReportAck"].is_string()) {
                                jMqtt.at("FaultReportAck").get_to(TOPIC_DVICE_VW_FAULT_ACK);
                                TOPIC_DVICE_VW_FAULT_ACK.insert(14, m_imei);
                            }

                            if (jMqtt["Ping"].is_string())// 配置ping ip
                            {
                                string pingIp;
                                jMqtt.at("Ping").get_to(pingIp);
                                PING::getInstance().setPingIp(pingIp);
                            }
                        }
                        broker.clientId = broker.clientId + "probe_" + m_imei;
                    }
                }
                // fin.close();

                // the Topics to subscribe， gyk test
                // if (!TOPIC_DVICE_VW_DATA_UP.empty()) {
                //     broker.topics.push_back(TOPIC_DVICE_VW_DATA_UP);
                // }
                if (!TOPIC_PLAT_ORDER_DOWN.empty()) {
                    broker.topics.push_back(TOPIC_PLAT_ORDER_DOWN);
                }
                if (!TOPIC_DVICE_VW_FAULT_ACK.empty()) {
                    broker.topics.push_back(TOPIC_DVICE_VW_FAULT_ACK);
                }

                LogInfo << "TOPIC_PLAT_ORDER_DOWN:" << TOPIC_PLAT_ORDER_DOWN;
                LogInfo << "TOPIC_DVICE_VW_DATA_UP:" << TOPIC_DVICE_VW_DATA_UP;
                LogInfo << "TOPIC_DVICE_VW_FAULT_UP:" << TOPIC_DVICE_VW_FAULT_UP;
                LogInfo << "TOPIC_DVICE_VW_FAULT_ACK:" << TOPIC_DVICE_VW_FAULT_ACK;


                LogInfo << "broker.address:" << broker.address;
                LogInfo << "broker.username:" << broker.username;
                LogInfo << "broker.password:" << broker.password;
                LogInfo << "broker.clientId:" << broker.clientId;
            }

        }

        void ProbeMgr::vwiseProccessTaskMgrHandle(sTaskMgrInfoPb &tm) {
            int result = 0;

            if (tm.p.address.empty()) {
                LogError << "ping address is empty";
                result = 1;
            } else if (tm.taskType != PingPb) {
                LogError << "unsupported v-wise task type:" << tm.taskType;
                result = 1;
            } else if (tm.taskControl == TaskStartPb) {
                if (PING::getInstance().isPingEnabled()) {
                    LogWarn << "ping task already running";
                    result = 1;
                } else {
                    LogDebug << "start ping task, address:" << tm.p.address;
                    PING::getInstance().start(tm.p.address);
                    std::string taskId = tm.taskId;
                    std::string targetIp = tm.p.address;
                    int execNum = tm.execNum > 0 ? tm.execNum : 1;
                    int frequency = tm.frequency > 0 ? tm.frequency : 1;
                    ProbeMgr::getInstance().m_timerPing.start(frequency * 1000, [taskId, targetIp, execNum]() mutable {
                        pingSendMsg(taskId, targetIp);
                        execNum--;
                        if (execNum <= 0) {
                            ProbeMgr::getInstance().m_timerPing.stop();
                            PING::getInstance().stop();
                            nlohmann::json j;
                            j["task_id"] = taskId;
                            j["result"] = 2;
                            LogDebug << "ping task completed: " << j.dump();
                        }
                    });
                }
            } else if (tm.taskControl == TaskStopPb) {
                if (PING::getInstance().isPingEnabled()) {
                    LogDebug << "stop ping task, address:" << tm.p.address;
                    ProbeMgr::getInstance().m_timerPing.stop();
                    PING::getInstance().stop();
                }
            } else {
                LogError << "unsupported ping task control:" << tm.taskControl;
                result = 1;
            }

            nlohmann::json j;
            j["task_id"] = tm.taskId;
            j["result"] = result;
            LogDebug << "task result: " << j.dump();
            if (!ProbeMgr::getInstance().TOPIC_PLAT_ORDER_DOWN_ACK.empty()) {
                ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_PLAT_ORDER_DOWN_ACK, j.dump());
            }

            return;
        }

        void ProbeMgr::proccessTaskMgrHandle(sTaskMgrInfoPb &tm) {
            int result = 0;
            bool found = false;
            int index = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(mtx);
                for (index = 0; index < ProbeMgr::getInstance().vTaskMgr.size(); index++) {
                    if (ProbeMgr::getInstance().vTaskMgr[index].taskId == tm.taskId) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    // LogInfo << "This is old task, taskControl:" << tm.taskControl;
                    if (ProbeMgr::getInstance().vTaskMgr[index].execNum > 0) {

                        if (tm.taskControl == TaskStartPb && ProbeMgr::getInstance().vTaskMgr[index].taskControl == TaskPausePb) {
                            ProbeMgr::getInstance().vTaskMgr[index].taskControl = TaskStartPb;
                            probeTaskStart(vTaskMgr[index]);
                        } else if (tm.taskControl == TaskStopPb) {
                            ProbeMgr::getInstance().vTaskMgr[index].execNum = 0;
                            probeTaskStop(vTaskMgr[index]);

                            std::string imei = ProbeMgr::getInstance().m_imei;
                            if (ProbeMgr::getInstance().vTaskMgr[index].uploadType == 0) {
#ifdef ENABLE_MINIO
                                if (MINIO::getInstance().minioUploadFile(ProbeMgr::getInstance().vTaskMgr[index].taskId, "/mnt/data/log/ProbeLog/", imei)) {
                                    std::string filePath = "/mnt/data/log/ProbeLog/" + ProbeMgr::getInstance().vTaskMgr[index].taskId + ".txt";
                                    remove(filePath.c_str());
                                }
#else
                                LogInfo << "MINIO upload disabled";
#endif
                            }
                            ProbeMgr::getInstance().vTaskMgr.erase(ProbeMgr::getInstance().vTaskMgr.begin() + index);
                        } else if (tm.taskControl == TaskPausePb) {
                            probeTaskStop(vTaskMgr[index]);
                            ProbeMgr::getInstance().vTaskMgr[index].taskControl = TaskPausePb;
                        } else {
                            LogError << "This is a old task, but taskControl:" << tm.taskControl;
                            result = 1;
                        }
                    } else {
                        LogInfo << "This task is stopping, passed this message";
                        result = 1;
                    }
                } else {
                    // LogInfo << "This is new task, taskControl:" << tm.taskControl << ", index:" << index;
                    bool foundTaskType = false;
                    for (int index1 = 0; index1 < ProbeMgr::getInstance().vTaskMgr.size(); index1++) {
                        if (vTaskMgr[index1].taskType == tm.taskType) {
                            foundTaskType = true;
                            break;
                        }
                    }
                    if (foundTaskType) {
                        LogInfo << "This is new task, but found same taskType not stop";
                        result = 1;
                    } else {
                        if (tm.taskControl == TaskStartPb) {
                            ProbeMgr::getInstance().vTaskMgr.push_back(tm);
                            probeTaskStart(tm);
                        } else {
                            LogError << "This is a new task, but taskControl:" << tm.taskControl;
                            result = 1;
                        }
                    }
                }
                // lock.unlock();
            }
            nlohmann::json j;
            j["task_id"] = tm.taskId;
            j["result"] = result;
            LogDebug << "task result: " << j.dump();
        }

        void ProbeMgr::probeTaskStart(sTaskMgrInfoPb &tm) {
            std::string taskId = tm.taskId;
            int uploadType = tm.uploadType;
            std::string imei = ProbeMgr::getInstance().m_imei;
            if (tm.taskType == PingPb) {
                PING::getInstance().start(tm.p.address);
                std::string targetIp = tm.p.address;
                ProbeMgr::getInstance().m_timerPing.start(tm.frequency * 1000, [taskId, targetIp, uploadType, imei] {
                    int execNum;
                    {
                        std::lock_guard<std::recursive_mutex> lock(ProbeMgr::getInstance().mtx);
                        execNum = ProbeMgr::getInstance().probeTaskSearch(taskId);
                    }
                    //lock.unlock();
                    if (execNum >= 0) {
                        pingSendMsg(taskId, targetIp);
                        if (execNum == 0) {
                            ProbeMgr::getInstance().m_timerPing.stop();
                            PING::getInstance().stop();
                            nlohmann::json j1;
                            j1["task_id"] = taskId;
                            if (uploadType == 0) {
#ifdef ENABLE_MINIO
                                if (MINIO::getInstance().minioUploadFile(taskId, "/mnt/data/log/ProbeLog/", imei)) {
                                    std::string filePath = "/mnt/data/log/ProbeLog/" + taskId + ".txt";
                                    remove(filePath.c_str());
                                    j1["status"] = 0;
                                } else {
                                    j1["status"] = 1;
                                }
#else
                                LogInfo << "MINIO upload disabled";
                                j1["status"] = 0;
#endif
                                std::string payload1 = j1.dump();
                                ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_PLAT_UPLOAD_DOWN_ACK, payload1);
                            }

                            nlohmann::json j;
                            j["task_id"] = taskId;
                            j["result"] = 2;
                            LogDebug << "task completed: " << j.dump();
                            {
                                std::lock_guard<std::recursive_mutex> lock(ProbeMgr::getInstance().mtx);
                                ProbeMgr::getInstance().probeTaskDelete(taskId);
                            }
                        }
                    } });
            }
            if (tm.taskType == WirelessPb) {
                // CellInfoCollector::getInstance().start(tm.frequency * 1000, taskId);
                ProbeMgr::getInstance().m_timerWireless.start(tm.frequency * 1000, [taskId, uploadType, imei] {
                    int execNum;
                    {
                        //std::unique_lock<std::mutex> lock(ProbeMgr::getInstance().mtx, std::defer_lock);
                        std::lock_guard<std::recursive_mutex> lock(ProbeMgr::getInstance().mtx);
                        //lock.lock();
                        execNum = ProbeMgr::getInstance().probeTaskSearch(taskId);
                        //lock.unlock();
                    }

                    if (execNum >= 0) {
                        wirelessSendMsg(taskId);
                        if (execNum == 0) {
                            ProbeMgr::getInstance().m_timerWireless.stop();
                            //CellInfoCollector::getInstance().stop();
                            nlohmann::json j1;
                            j1["task_id"] = taskId;
                            if (uploadType == 0) {
#ifdef ENABLE_MINIO
                                if (MINIO::getInstance().minioUploadFile(taskId, "/mnt/data/log/ProbeLog/", imei)) {
                                    std::string filePath = "/mnt/data/log/ProbeLog/" + taskId + ".txt";
                                    remove(filePath.c_str());
                                    j1["status"] = 0;
                                } else {
                                    j1["status"] = 1;
                                }
#else
                                LogInfo << "MINIO upload disabled";
                                j1["status"] = 0;
#endif
                                std::string payload1 = j1.dump();
                                ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_PLAT_UPLOAD_DOWN_ACK,
                                                                         payload1);
                            }
                            nlohmann::json j;
                            j["task_id"] = taskId;
                            j["result"] = 2;
                            LogDebug << "task completed: " << j.dump();
                            {
                                std::lock_guard<std::recursive_mutex> lock(ProbeMgr::getInstance().mtx);
                                ProbeMgr::getInstance().probeTaskDelete(taskId);
                            }
                        }
                    } });
            }

        }

        void ProbeMgr::probeTaskStop(sTaskMgrInfoPb &tm) {
            if (tm.taskType == PingPb) {
                ProbeMgr::getInstance().m_timerPing.stop();
                PING::getInstance().stop();
            }
            if (tm.taskType == WirelessPb) {
                // CellInfoCollector::getInstance().stop();
                ProbeMgr::getInstance().m_timerWireless.stop();
            }
        }

        void ProbeMgr::proccessZDFaultAck(std::string input, std::string topic) {
            clearFault();
        }

        void ProbeMgr::proccessDataReport(std::string input, std::string topic) {
            LogInfo << "proccessDataReport received, topic: " << topic << ", input: " << input;
            // Handle data report message from platform
        }
        void ProbeMgr::proccessTaskMgr(std::string input, std::string topic) {
            LogInfo << "input:" << input;
            sTaskMgrInfoPb tm;
            if (nlohmann::json::accept(input)) {
                nlohmann::json j = nlohmann::json::parse(input);
                if (j.is_discarded()) {
                    LogInfo << "input data is not right json ..., " << topic;
                } else {
                    // 采集配置指令分流：cmd_type=="collection_config" 走采集配置处理，不影响既有任务指令
                    if (j.contains("cmd_type") && j["cmd_type"].is_string() &&
                        j["cmd_type"].get<std::string>() == "collection_config") {
                        proccessCollectionConfig(j);
                        return;
                    }
                    if (j["task_id"].is_string())
                        j.at("task_id").get_to(tm.taskId);

                    if (j["task_type"].is_number_integer())
                        tm.taskType = j["task_type"];

                    if (j["task_control"].is_number_integer())
                        tm.taskControl = j["task_control"];

                    if (j["exec_num"].is_number_integer())
                        tm.execNum = j["exec_num"];

                    if (j["frequency"].is_number_integer())
                        tm.frequency = j["frequency"];

                    if (j["upload_type"].is_number_integer())
                        tm.uploadType = j["upload_type"];

                    if (j["paras"].is_object()) {
                        nlohmann::json j1 = j["paras"];

                        if (j1["packet_size"].is_number_integer())
                            tm.p.packetSize = j1["packet_size"];

                        if (j1["address"].is_string())
                            j1.at("address").get_to(tm.p.address);

                        if (j1["timeout"].is_number_integer())
                            tm.p.timeout = j1["timeout"];

                        if (j1["noNet5GThr"].is_number_integer()) {
                            tm.p.noNet5GThr = j1["noNet5GThr"];
                            m_noNet5GThr = tm.p.noNet5GThr;
                            LogDebug<<"set noNet5GThr: "<<tm.p.noNet5GThr;
                        }

                        if (j1["noNetLTEThr"].is_number_integer()) {
                            tm.p.noNetLTEThr = j1["noNetLTEThr"];
                            m_noNetLTEThr = tm.p.noNetLTEThr;
                            LogDebug<<"set noNetLTEThr: "<<tm.p.noNetLTEThr;
                        }
                    }
                    // ProbeMgr::getInstance().proccessTaskMgrHandle(tm);
                    ProbeMgr::getInstance().vwiseProccessTaskMgrHandle(tm);
                }
            } else {
                LogDebug << "command message, format error";
            }

            return;
        }

        // 解析 sys/commands 下发的采集配置指令，动态调整上传内容与频率
        // 指令格式：{"cmd_type":"collection_config","vid":...,"category":"full|core","interval_sec":N,"retention_days":N,"name":...}
        void ProbeMgr::proccessCollectionConfig(const nlohmann::json &j) {
            // imei 非空且与本设备 imei 不符则忽略
            if (j.contains("imei") && j["imei"].is_string()) {
                std::string imei = j["imei"].get<std::string>();
                if (!imei.empty() && imei != m_imei) {
                    LogInfo << "collection_config ignored, imei mismatch: cmd=" << imei << ", device=" << m_imei;
                    return;
                }
            }

            // 配置名称（仅日志）
            std::string name;
            if (j.contains("name") && j["name"].is_string()) {
                name = j["name"].get<std::string>();
            }

            // 数据采集类别
            bool categoryChanged = false;
            if (j.contains("category") && j["category"].is_string()) {
                std::string cat = j["category"].get<std::string>();
                if (cat == "full") {
                    m_collectCategory = CollectFull;
                    categoryChanged = true;
                } else if (cat == "core") {
                    m_collectCategory = CollectCore;
                    categoryChanged = true;
                } else {
                    LogInfo << "collection_config unknown category: " << cat << ", keep current";
                }
            }

            // 采集频率（秒）
            bool intervalChanged = false;
            if (j.contains("interval_sec") && j["interval_sec"].is_number_integer()) {
                int sec = j["interval_sec"].get<int>();
                if (sec > 0) {
                    m_collectIntervalMs = sec * 1000;
                    intervalChanged = true;
                } else {
                    LogInfo << "collection_config invalid interval_sec: " << sec << ", keep current";
                }
            }

            // 数据保留（仅日志，SDK 端不实现）
            if (j.contains("retention_days") && j["retention_days"].is_number_integer()) {
                LogInfo << "collection_config retention_days=" << j["retention_days"].get<int>() << " (recorded only)";
            }

            // 频率变更需重启主采集定时器
            if (intervalChanged) {
                restartCollectionTimer();
            }

            LogInfo << "collection_config applied: name=" << name
                    << ", category=" << (m_collectCategory == CollectFull ? "full" : "core")
                    << ", interval_ms=" << m_collectIntervalMs
                    << ", categoryChanged=" << categoryChanged
                    << ", intervalChanged=" << intervalChanged;
        }

        // 按当前 m_collectIntervalMs 重启主采集定时器（Timer::start 不可重入，需先 stop）
        void ProbeMgr::restartCollectionTimer() {
            m_timerZD.stop();
            m_timerZD.start(m_collectIntervalMs, [this] { vwiseProbeTaskHandle(); });
            LogInfo << "collection timer restarted, interval_ms=" << m_collectIntervalMs;
        }
        void ProbeMgr::proccessSwitch(const std::string &input, const std::string &topic) {
            // LogInfo << "input:" << input << ", topic: " << topic;
            nlohmann::json jAck;
            if (nlohmann::json::accept(input)) {
                nlohmann::json j = nlohmann::json::parse(input);
                int type;
                if (j.is_discarded()) {
                    LogWarn << "input data is not right json ..., " << input;
                    jAck["status"] = 1;
                } else {
                    if (j["type"].is_number_integer())
                        type = j["type"];
                    nlohmann::json rwJson(common::GlobalData::Instance()->getRwJson());
                    LogDebug << "rwJson: " << rwJson.dump();
                    if (0 == type || 1 == type) {
                        rwJson["Obu"]["UUorPC5"] = type;
                    }
                    common::GlobalData::Instance()->setRwJson(rwJson);
                    common::GlobalData::Instance()->writeToFile(rwJson);
                    jAck["status"] = 0;
                }
            } else {
                LogWarn << "input data is not right json ..., " << input;
                jAck["status"] = 1;
            }
            std::string payload1 = jAck.dump();
            ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_USER_SWITCHING_CHANNELS_ACK,
                                                     payload1);
        }

        void ProbeMgr::proccessUserOnline(std::string input, std::string topic) {
            if (nlohmann::json::accept(input)) {
                nlohmann::json j = nlohmann::json::parse(input);
                if (j.is_discarded()) {
                    LogInfo << "input data is not right json ..., " << topic;
                } else {
                    ProbeMgr::getInstance().m_timerUser.stop();
                }
            }
        }

        void ProbeMgr::proccessUploadFile(std::string input, std::string topic) {
            if (nlohmann::json::accept(input)) {
                nlohmann::json j = nlohmann::json::parse(input);
                std::string taskId;
                if (j.is_discarded()) {
                    LogInfo << "input data is not right json ..., " << topic;
                } else {
                    if (j["task_id"].is_string())
                        j.at("task_id").get_to(taskId);
                    std::string filename = taskId + ".txt";
                    std::string path = "/mnt/data/log/ProbeLog";

                    nlohmann::json j1;
                    j1["task_id"] = taskId;

                    if (base_tools::util::fileExistsInFolder(path, filename)) {
                        // LogInfo << "Found the file ...";
                        std::string imei = ProbeMgr::getInstance().m_imei;
#ifdef ENABLE_MINIO
                        if (MINIO::getInstance().minioUploadFile(taskId, "/mnt/data/log/ProbeLog/", imei)) {
                            std::string filePath = "/mnt/data/log/ProbeLog/" + taskId + ".txt";
                            remove(filePath.c_str());
                            j1["status"] = 0;
                        }
#else
                        LogInfo << "MINIO upload disabled";
                        j1["status"] = 0;
#endif
                    } else {
                        j1["status"] = 1;
                        LogInfo << "Not found the file ...";
                    }

                    std::string payload1 = j1.dump();
                    ProbeMgr::getInstance().mqtt.messageSend(ProbeMgr::getInstance().TOPIC_PLAT_UPLOAD_DOWN_ACK,
                                                             payload1);
                }
            }
        }

        int ProbeMgr::receiveMsgHandler(void *context, char *topicName, int topicLen, MQTTAsync_message *message) {
            std::string input((char *) message->payload, message->payloadlen);
            std::string topicStr((char *) topicName, topicLen);
            // LogInfo << "receive message with UU: " << topicStr << ", message: " << input;

            for (std::map<std::string, std::function<void(std::string, std::string)>>::iterator iter =
                         ProbeMgr::getInstance().handleMap.begin();
                 iter != ProbeMgr::getInstance().handleMap.end(); ++iter) {
                if (topicStr.find(iter->first) != string::npos) {
                    iter->second(input, topicStr);
                    break;
                }
            }

            MQTTAsync_freeMessage(&message);
            MQTTAsync_free(topicName);
            return 1;
        }

        void ProbeMgr::probeMgrStart() {
            handleMap = {
                    {"/sys/commands", [this](std::string input, std::string topic) { proccessTaskMgr(input, topic); }},
                    {"/user/mgr/up/ack", [this](std::string input, std::string topic) { proccessUserOnline(input, topic); }},
                    //just for test, gyk
                    //{"probe/devices/868371055248377/data/up", [this](std::string input, std::string topic) { proccessDataReport(input, topic); }},
                    {"ack", [this](std::string input, std::string topic) { proccessZDFaultAck(input, topic); }}};
            // base_tools::util::creatFilePath("/mnt/data/log/ProbeLog");

            readJsonFile("/etc/rw.conf");
            mqtt.start(ProbeMgr::receiveMsgHandler, broker);

            // 上线
            // m_timerUser.start(5 * 1000, [] { userOnlineSendMsg(); });

            // CellInfoCollector::getInstance().start(1000);

            // ping启动
            // PING::getInstance().start(PING::getInstance().getPingIp());
            // rpi 初始化
            rpi::RPIModuleInterface::getInstance().init();

            m_timerZD.start(3 * 1000, [this] { vwiseProbeTaskHandle(); });

        }

        void ProbeMgr::probeMgrStop() {
            LogInfo << "probeMgrStop ...";
            PING::getInstance().stop();
            rpi::RPIModuleInterface::getInstance().deinit();
            m_timerUser.stop();
            m_timerHB.stop();
            mqtt.stop();
        }
    }// end namespace vwise
}// end namespace cmsr
