/**********************************************************************************************************************
    > File Name: cell_info.cpp
    > Author: htsong
    > Date: 12/12/23
**********************************************************************************************************************/

#ifndef CMSR_VWISE_CELL_INFO_H
#define CMSR_VWISE_CELL_INFO_H

#include <string>
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>
//#include "mqtt_async_publisher.h"

namespace cmsr {
    namespace vwise {
        class CellInfoCollector {
        public:
            void getCellInfo();

            void start(long long interval);
            void stop();
            void getCellInfoLoop(long long interval);

            std::string m_strJ;
            std::mutex mutex_;

        private:
            std::atomic<bool> bRunning_{true};
            std::unique_ptr<std::thread> cellInfoThd_{nullptr};
//            MqttAsyncPublisher mqttSender_;
            std::string m_taskId;
        public:
            static CellInfoCollector &getInstance();

            const std::string &GetMStrJ();
            void SetMStrJ(const std::string &m_str_j);

        private:
            CellInfoCollector();

            CellInfoCollector(const CellInfoCollector &) = delete;

            CellInfoCollector &operator=(const CellInfoCollector &) = delete;
        };
    } //end namespace vwise
} //end namespace cmsr

#endif //CMSR_VWISE_CELL_INFO_H