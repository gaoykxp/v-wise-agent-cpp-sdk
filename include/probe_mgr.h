/**********************************************************************************************************************
    > File Name: probe_mgr.h
    > Author: hrliu dmhuang
    > Date: 12/20/23
**********************************************************************************************************************/

#ifndef TANGO_NEXUS_PROBE_MGR_H
#define TANGO_NEXUS_PROBE_MGR_H

#include "rpi_cell_info.h"
#include "mqtt_async_app.h"
#include "offline_cache.h"
#include "ping.h"
#include <atomic>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <time.h>
#include <vector>


namespace cmsr {
    namespace vwise {
        enum TaskControlPb {
            TaskStartPb = 0,
            TaskStopPb,
            TaskPausePb
        };
        enum TaskTypePb {
            PingPb = 0,
            WirelessPb,
            Pc5Pb,
            BsmTaskTypePb,
            ZDIndexPb = 100
        };
        // SDK数据采集类别：全量网络数据 / 核心网络数据
        enum CollectCategory {
            CollectFull = 0,
            CollectCore = 1
        };

        typedef struct {
            int packetSize;
            std::string address;
            int timeout;
            int noNet5GThr;
            int noNetLTEThr;
        } sParasInfoPb;

        typedef struct {
            std::string taskId;
            int taskType;
            int taskControl;
            int execNum;
            int frequency = 1;
            int uploadType;
            sParasInfoPb p;
        } sTaskMgrInfoPb;

        class ProbeMgr {
        public:
            void probeMgrStart();
            void probeMgrStop();

        private:
            void readJsonFile(const std::string &cfgPath);
            static int receiveMsgHandler(void *context, char *topicName, int topicLen, MQTTAsync_message *message);
            void proccessTaskMgr(std::string input, std::string topic);
            void proccessSwitch(const std::string &input, const std::string &topic);
            void proccessUserOnline(std::string input, std::string topic);
            void proccessUploadFile(std::string input, std::string topic);
            void probeTaskStart(sTaskMgrInfoPb &tm);
            void probeTaskStop(sTaskMgrInfoPb &tm);
            void probeTaskDelete(std::string taskId);
            int probeTaskSearch(std::string taskId);
            void proccessTaskMgrHandle(sTaskMgrInfoPb &tm);
            void proccessZDFaultAck(std::string input, std::string topic);
            void proccessDataReport(std::string input, std::string topic); //only for test, GYK

            // 周期采集数据上报（带断连缓存）：在线直发并触发积压补传；断网落盘缓存
            void publishData(const std::string& payload);
            // 后台逐条回传本地缓存（网络恢复后调用）
            void flushOfflineCache();

            void vwiseProccessTaskMgrHandle(sTaskMgrInfoPb &tm);
            void vwiseProbeTaskHandle();
            // 解析 sys/commands 下发的采集配置指令，动态调整上传内容与频率
            void proccessCollectionConfig(const nlohmann::json &j);
            // 按当前 m_collectIntervalMs 重启主采集定时器
            void restartCollectionTimer();

        public:
            MQTTAPP mqtt;
            std::vector<sTaskMgrInfoPb> vTaskMgr;
            std::recursive_mutex mtx;
            std::string m_imei;
            MqttBroker broker;

            std::string TOPIC_USER_ONLINE;
            std::string TOPIC_USER_ONLINE_ACK;
            std::string TOPIC_DVICE_PING_UP;
            std::string TOPIC_DVICE_WIRELESS_UP;
            std::string TOPIC_PLAT_ORDER_DOWN;
            std::string TOPIC_PLAT_ORDER_DOWN_ACK;
            std::string TOPIC_PLAT_UPLOAD_DOWN;
            std::string TOPIC_PLAT_UPLOAD_DOWN_ACK;
            std::string TOPIC_USER_HEARTBEAT;
            std::string TOPIC_USER_SWITCHING_CHANNELS;
            std::string TOPIC_USER_SWITCHING_CHANNELS_ACK;
            std::string TOPIC_DVICE_VW_DATA_UP;  //index up for v-wise
            std::string TOPIC_DVICE_VW_FAULT_UP; //index up for v-wise
            std::string TOPIC_DVICE_VW_FAULT_ACK;//index up for v-wise

        public:
            static ProbeMgr &getInstance();

        private:
            ProbeMgr();
            ProbeMgr(const ProbeMgr &) = delete;
            ProbeMgr &operator=(const ProbeMgr &) = delete;
            ~ProbeMgr();

        private:
            Timer m_timerPing;
            Timer m_timerWireless;
            Timer m_timerUser;
            Timer m_timerHB;
            Timer m_timerZD;

            std::map<std::string, std::function<void(std::string, std::string)>> handleMap;

            //noNet threshold
            int m_noNet5GThr = -1;
            int m_noNetLTEThr = -1;

            //SDK数据采集运行时配置（由 sys/commands 下发的 collection_config 指令动态调整）
            CollectCategory m_collectCategory = CollectFull; // 默认全量
            int m_collectIntervalMs = 3000;                  // 默认3秒，与原定时器一致

            //周期采集数据断连重传
            OfflineCache m_offlineCache;            // 断网本地缓存
            std::atomic<bool> m_flushing{false};    // 补传进行中标志（防重入）
            bool m_cacheEnabled = true;             // 是否启用断连缓存（配置开关）

        };
    }//end namespace vwise
}//end namespace cmsr

#endif//TANGO_NEXUS_PROBE_MGR_H
