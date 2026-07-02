/**********************************************************************************************************************
    > File Name: cell_info_stub.cpp
    > Author: Generated for Raspberry Pi
    > Date: 03/06/26
    > Description: Stub implementation for CellInfo without zdapi dependency
**********************************************************************************************************************/
#include "rpi_cell_info.h"
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <random>

#include "log.h"
#include "util.h"
#include "base_timer.h"
#include "nlohmann/json.hpp"

namespace {
    using json = nlohmann::json;

    json formJInfo() {
        json j_info;

        // Simulated cell info for Raspberry Pi (no real cellular module)
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> rsrp_dist(-100, -50);
        static std::uniform_int_distribution<> rsrq_dist(-20, -5);
        static std::uniform_int_distribution<> sinr_dist(-5, 25);

        j_info["rsrp"] = std::to_string(rsrp_dist(gen));
        j_info["rsrq"] = std::to_string(rsrq_dist(gen));
        j_info["sinr"] = std::to_string(sinr_dist(gen));
        j_info["rat"] = "LTE";  // Simulated LTE connection
        j_info["cid"] = "12345678";  // Simulated cell ID
        j_info["tac"] = "1234";  // Simulated TAC
        j_info["state"] = 1;  // Registered

        return j_info;
    }
}

namespace cmsr {
    namespace vwise {
        using namespace std;
        using json = nlohmann::json;

        CellInfoCollector &CellInfoCollector::getInstance() {
            static CellInfoCollector m_instance;
            return m_instance;
        }

        CellInfoCollector::CellInfoCollector() = default;

        void CellInfoCollector::getCellInfo() {
            LogInfo << "[RPi Stub] getCellInfo (simulated data)";

            json j_out;
            j_out["time"] = base_tools::util::sysTimeMqtt();
            j_out["cell_info"][0] = formJInfo();

            try {
                string jsonStr = j_out.dump();
                SetMStrJ(jsonStr);
            }
            catch (const exception& e) {
                LogWarn << "from json error: " << e.what();
            }
            catch (...) {
                LogWarn << "from json error: ";
            }
        }

        void CellInfoCollector::getCellInfoLoop(long long interval) {
            while (bRunning_) {
                getCellInfo();
                std::this_thread::sleep_for(std::chrono::milliseconds(interval));
            }
        }

        void CellInfoCollector::start(long long interval) {
            LogInfo << "[RPi Stub] CellInfoCollector start (simulated data)";
            bRunning_.store(true);
            if (!cellInfoThd_) {
                cellInfoThd_ = make_unique<thread>([this, interval]() {
                    getCellInfoLoop(interval);
                });
            }
            LogInfo << "CellInfoCollector start end.";
        }

        void CellInfoCollector::stop() {
            LogInfo << "CellInfoCollector stop begin.";
            bRunning_.store(false);
            if (cellInfoThd_) {
                cellInfoThd_->join();
                cellInfoThd_ = nullptr;
            }
            LogInfo << "CellInfoCollector stop end.";
        }

        const string &CellInfoCollector::GetMStrJ() {
            std::lock_guard<std::mutex> lg(mutex_);
            return m_strJ;
        }

        void CellInfoCollector::SetMStrJ(const string &m_str_j) {
            std::lock_guard<std::mutex> lg(mutex_);
            m_strJ = m_str_j;
        }

    }  // end namespace vwise
}  // end namespace cmsr