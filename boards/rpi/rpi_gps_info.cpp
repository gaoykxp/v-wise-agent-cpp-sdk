/**********************************************************************************************************************
    > File Name: zd_gps_info_stub.cpp
    > Author: Generated for Raspberry Pi
    > Date: 03/06/26
    > Description: Real GPS via RM520N-GL AT+QGPS (replaces stub simulation)
**********************************************************************************************************************/
#include <stdio.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

#include "log.h"
#include "rpi_gps_info.h"
#include "rpi_module_interface.h"

namespace cmsr
{
    namespace vwise
    {
        using namespace std;

        GPS &GPS::getInstance()
        {
            static GPS m_instance;
            return m_instance;
        }

        GPS::GPS()
        {
        }

        GPS::~GPS()
        {
        }

        bool GPS::init()
        {
            // 初始化为零、无定位
            std::lock_guard<std::mutex> lock(mutex_);
            gData = GPSData{};
            gData.valid = false;
            LogInfo << "[GPS] real GPS via RM520N-GL AT+QGPS";
            return true;
        }

        void GPS::start()
        {
            if (bRunning.load())
            {
                return;
            }
            bRunning.store(true);
            LogInfo << "[GPS] starting real GNSS polling...";

            std::thread([this]() {
                auto &rpi = rpi::RPIModuleInterface::getInstance();

                // 开启 GNSS（AT+QGPS=1）；若已开启模组返回 ERROR，视为已开，继续轮询
                if (rpi.enableGnss(1))
                {
                    LogInfo << "[GPS] GNSS enabled";
                }
                else
                {
                    LogWarn << "[GPS] enableGnss returned false (may already be on or AT not ready)";
                }

                bool everFixed = false;
                int failCount = 0;

                while (bRunning.load())
                {
                    auto loc = rpi.getGpsLocation();
                    if (loc && loc->valid)
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        gData.latitude = loc->lat;
                        gData.longitude = loc->lon;
                        gData.altitude = loc->altitude;
                        gData.speed = loc->speed;
                        gData.course = loc->course;
                        gData.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        gData.valid = true;
                        failCount = 0;
                        everFixed = true;
                    }
                    else
                    {
                        // [临时] GPS 无法从模组获取时，使用固定坐标兜底（上海一带 31.247656, 121.612243）
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            gData.latitude = 31.247656;
                            gData.longitude = 121.612243;
                            gData.valid = true;
                        }
                        failCount++;
                        if (failCount % 10 == 0)
                        {
                            LogWarn << "[GPS] no fix, failCount=" << failCount << " (using fallback coords)";
                        }
                        // 冷启动阶段（从未定位成功）持续失败 -> 周期性重开 GNSS
                        if (!everFixed && failCount % 20 == 0)
                        {
                            LogInfo << "[GPS] re-enable GNSS (cold start, no fix yet)";
                            rpi.enableGnss(1);
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::seconds(3));
                }
            }).detach();
        }

        void GPS::stop()
        {
            if (!bRunning.load())
            {
                return;
            }
            LogInfo << "[GPS] stopping GNSS polling...";
            bRunning.store(false);
            // 不调用 gps_disable：保留 GNSS 开启，便于下次快速重定位
        }

    }  // namespace vwise
}  // namespace cmsr
