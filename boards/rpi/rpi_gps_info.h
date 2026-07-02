#ifndef CMSR_VWISE_GPS_INFO_H
#define CMSR_VWISE_GPS_INFO_H
#include <iostream>
#include <thread>
#include <atomic>
#include <thread>
#include <mutex>

namespace cmsr {
    namespace vwise {

		typedef struct _GPSData {
			double latitude;
			double longitude;
			double altitude;    /* unit: m   */
			double speed;       /* unit: m/s */
			uint64_t timestamp;
			double course;
		}GPSData;

		class GPS {
		public:
			// bool init(const char* socket_path);
			bool init();
			void start();
			void stop();

            std::mutex mutex_;
			GPSData gData;
        public:
            static GPS &getInstance();

        private:
			std::string socket_path_;
            int socket_fd_;

			std::atomic<bool> bRunning{false};
            std::unique_ptr<std::thread> gpsThd{nullptr};
		
		public:
            GPSData receivedData;

		private:
            GPS();

            GPS(const GPS &) = delete;

            GPS &operator=(const GPS &) = delete;

            ~GPS();
		};
	}
}


#endif


