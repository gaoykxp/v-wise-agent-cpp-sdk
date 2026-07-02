#include <fstream>
#include <sstream>
#include <unistd.h>
#include "ping.h"
#include <chrono>
#include "log.h"


namespace cmsr {
    namespace vwise {
        using namespace std;

        PING &PING::getInstance() {
            static PING m_instance;
            return m_instance;
        }

        PING::PING() {
            ;
        }
        PING::~PING() {
            ;
        }
        bool PING::isPingEnabled(void)
        {
            if(m_pingLossRunning || m_pingTimeRunning)
            {
                return true;
            }
            return false;
        }

        int PING::getPingTime()
        {
            while (m_pingTimeRunning)
            {
                // auto start = std::chrono::system_clock::now();
                std::ostringstream commandStream;
                commandStream << "ping -c 1 -W 1 " << m_ip << " > ping_tmp.txt";
                std::system(commandStream.str().c_str());

                std::ifstream file("ping_tmp.txt");
                if (!file.good())
                {
                    LogError << "Failed to open ping result file";
                    file.close();
                    return -1;
                }
                m_pingTime = 0;
                std::string line;
                bool isFind = false;
                while (std::getline(file, line))
                {
                    if (line.find("time=") != std::string::npos)
                    {
                        int startIndex = line.find("time=") + 5;
                        int endIndex = line.find(" ms", startIndex);
                        // std::cout << startIndex << " " << endIndex << std::endl;
                        m_pingTime = std::stoi(line.substr(startIndex, endIndex - startIndex));
                        isFind = true;
                        break;
                    }
                }

                if(!isFind)
                {
                    m_pingTime = -1;
                }

                LogInfo << "m_pingTime:" << m_pingTime << "ms";
                if (m_pingTime < 1000)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000 - m_pingTime));
                }
                file.close();
                // auto end = std::chrono::system_clock::now();

                // auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                // std::cout <<" rrt time : " << duration.count()  << " ms\n";
            }
            return 0;
        }

        int PING::getPingLossPacket()
        {
            while (m_pingLossRunning)
            {
                // auto start = std::chrono::system_clock::now();
                std::ostringstream commandStream;
                commandStream << "ping -c 3 -W 1 " << m_ip << " > ping_loss.txt"; //-w 0.3
                std::system(commandStream.str().c_str());

                std::ifstream file("ping_loss.txt");
                if (!file.good())
                {
                    LogError << "Failed to open ping result file";
                    file.close();
                    return -1;
                }
                // auto end = std::chrono::system_clock::now();

                // auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                // std::cout <<" loss time : " << duration.count()  << " ms\n";

                std::string line;
                bool isFind = false;
                while (std::getline(file, line))
                {
                    if (line.find("received") != std::string::npos)
                    {
                        int startIndex = line.find("received,") + 10;
                        int endIndex = line.find("%", startIndex);
                        // std::cout << startIndex << " " << endIndex << std::endl;
                        m_loss = std::stoi(line.substr(startIndex, endIndex - startIndex));
                        isFind = true;
                        break;
                    }
                }
                if(!isFind)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                }
                LogInfo << "m_loss:" << m_loss << "%";
                
                file.close();
            }

            return 0;
        }

        void PING::start(const std::string &ip)
        {
            m_ip = ip;
            m_pingLossRunning = true;
            m_pingTimeRunning = true;

            if (!m_pLossThd)
            {
                m_pLossThd = make_unique<thread>([this]()
                                                 { getPingLossPacket(); });
            }

            if (!m_pTimeThd)
            {
                m_pTimeThd = make_unique<thread>([this]()
                                                 { getPingTime(); });
            }
        }

        void PING::stop()
        {
            m_pingLossRunning = false;
            m_pingTimeRunning = false;

            if (m_pTimeThd)
            {
                m_pTimeThd->join();
                m_pTimeThd = nullptr;
            }

            if (m_pLossThd)
            {
                m_pLossThd->join();
                m_pLossThd = nullptr;
            }
        }
        void PING::setPingIp(const std::string &ip)
        {
            m_ip = ip;
        }

        std::string PING::getPingIp(void)
        {
            return m_ip;
        }
    }
}
