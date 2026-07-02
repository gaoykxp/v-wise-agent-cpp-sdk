#ifndef TANGO_NEXUS_PING_H
#define TANGO_NEXUS_PING_H
#include <iostream>
#include <thread>
#include <atomic>

namespace cmsr {
    namespace vwise {
		class PING {
		public:
			void start(const std::string& ip);
			void stop();
			void setPingIp(const std::string& ip);
			std::string getPingIp(void);
			bool isPingEnabled(void);

			
			std::atomic_int m_loss;
			std::atomic_int m_pingTime;

        private:
			int getPingTime();
			int getPingLossPacket();

			bool m_pingLossRunning{false};
			bool m_pingTimeRunning{false};
			std::string m_ip;

			std::unique_ptr<std::thread> m_pLossThd{nullptr};
			std::unique_ptr<std::thread> m_pTimeThd{nullptr};
		
		public:
            static PING &getInstance();

	    
        private:
            PING();

            PING(const PING &) = delete;

            PING &operator=(const PING &) = delete;

            ~PING();
		};
	}
}


#endif


