/**********************************************************************************************************************
    > File Name: zd_gps_info_stub.cpp
    > Author: Generated for Raspberry Pi
    > Date: 03/06/26
    > Description: Stub implementation for GPS without zdapi dependency
**********************************************************************************************************************/
#include <stdio.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <random>

#include "log.h"
#include "rpi_gps_info.h"

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
            LogInfo << "[RPi Stub] GPS initialized (stub mode - no real GPS hardware)";
            return true;
        }

        void GPS::start()
        {
            if (bRunning.load())
            {
                return;
            }
            LogDebug << "[RPi Stub] Starting GPS simulation...";
            bRunning.store(true);

            // Start a simulation thread that generates random GPS data
            std::thread([this]() {
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_real_distribution<> lat_dist(31.247656, 31.247676);
                std::uniform_real_distribution<> lon_dist(121.612243, 121.612263);
                std::uniform_real_distribution<> alt_dist(10.0, 100.0);
                std::uniform_real_distribution<> speed_dist(0.0, 30.0);
                std::uniform_real_distribution<> course_dist(0.0, 360.0);

                while (bRunning.load())
                {
                    std::lock_guard<std::mutex> lock(mutex_);

                    // Generate simulated GPS coordinates (Beijing area)
                    gData.latitude = lat_dist(gen);
                    gData.longitude = lon_dist(gen);
                    gData.altitude = alt_dist(gen);
                    gData.speed = speed_dist(gen);
                    gData.course = course_dist(gen);
                    gData.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }).detach();
        }

        void GPS::stop()
        {
            if (!bRunning.load())
            {
                return;
            }
            LogDebug << "[RPi Stub] Stopping GPS simulation...";
            bRunning.store(false);
        }

    }  // namespace vwise
}  // namespace cmsr