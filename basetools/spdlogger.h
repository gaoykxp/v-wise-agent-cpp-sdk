/**********************************************************************************************************************
    > File Name: spdlogger.h
    > Author: htsong
    > Date: 5/12/23
**********************************************************************************************************************/

#pragma once

#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <memory>
#include <sstream>
#include <iostream>
//
#include "spdlog/spdlog.h"
#include "spdlog/async.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"


namespace{
    const std::string EMPTY_STR = "";

    std::string getProgramPath() {
        char path[PATH_MAX] = {0};
        auto len = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (len == -1) {
            return EMPTY_STR;
        }
        path[len] = '\0';
        return std::string(path);
    }

    std::string getExeName() {
        std::string path = getProgramPath();
        return path.substr(path.find_last_of("/") + 1);
    }
}

namespace cmsr {
    namespace logger {

        namespace {
            inline std::string getEnv(const std::string& var_name,
                                      const std::string& default_value = "") {
                const char* var = std::getenv(var_name.c_str());
                if (var == nullptr) {
                    std::cout << "Environment variable [" << var_name << "] not set, fallback to "
                          << default_value << std::endl;
                    return default_value;
                }
                return std::string(var);
            }
        }

        template<typename LogType>
        class Logger;

        template<>
        class Logger<spdlog::logger> {
        public:
            static Logger<spdlog::logger> &getInstance() {
                static Logger<spdlog::logger> m_instance;
                return m_instance;
            };

            Logger() {
                try {
                    console_logger_ = spdlog::stdout_color_mt("consoleLogger");
                    console_logger_->set_pattern(
                            "[%Y-%m-%d %H:%M:%S.%f] <%t> [%^%l%$] [%s:%!:%#] %v");
//                    console_logger_->set_level(spdlog::level::debug);
//                    console_logger_->flush_on(spdlog::level::debug);
                    std::string log_level = getEnv("TANGO_LOG_LEVEL", "1");
                    console_logger_->set_level(static_cast<spdlog::level::level_enum>(std::stoi(log_level)));
                    console_logger_->flush_on(static_cast<spdlog::level::level_enum>(std::stoi(log_level)));
                }
                catch (const spdlog::spdlog_ex &ex) {
                    std::cerr << "Logger initialization failed: " << ex.what() << std::endl;
                }
            }

            ~Logger() {
                spdlog::drop("consoleLogger");
            }

            std::shared_ptr<spdlog::logger> GetLogger() {
                return console_logger_;
            }

        private:
            Logger(const Logger &) = delete;

            Logger(Logger &&) = delete;

            Logger &operator=(const Logger &) = delete;

            Logger &operator=(Logger &&) = delete;

        private:
            std::shared_ptr<spdlog::logger> console_logger_;
        };

        template<>
        class Logger<spdlog::async_logger> {
        public:
            static Logger<spdlog::async_logger> &getInstance() {
                static Logger<spdlog::async_logger> m_instance;
                return m_instance;
            };

            Logger() {
                is_inited_.store(false);
                init();
            }

            ~Logger() {
                spdlog::drop("fileLogger");
            }

            std::shared_ptr<spdlog::logger> GetLogger() {
                return file_logger_;
            }

        private:
            Logger(const Logger &) = delete;

            Logger(Logger &&) = delete;

            Logger &operator=(const Logger &) = delete;

            Logger &operator=(Logger &&) = delete;

        public:
            bool init() {
//                std::string binary_name = apollo::cyber::binary::GetName();
//                if (binary_name.empty())
//                    return false;
                time_t now;
                time(&now);
                char buff[32] = {0};
                const std::string TIME_FORMAT = "%Y%m%d-%H%M%S.";
                strftime(buff, sizeof(buff), TIME_FORMAT.c_str(), localtime(&now));
                std::string log_file_path = "./log/";
//                log_file_path.append(binary_name);
                log_file_path.append(getExeName());
                log_file_path.append(".log");
                // log_file_path.append(std::string(buff));
                // log_file_path.append(std::to_string(getpid()));
                try {
                    file_logger_ =
                            spdlog::create_async<spdlog::sinks::rotating_file_sink_mt>(
                                    "fileLogger",
                                    log_file_path,
                                    20 * 1024 * 1024,
                                    5);
                    // timestamp, thread_id, filename and line number.
                    file_logger_->set_pattern(
                            "[%Y-%m-%d %H:%M:%S.%f] <%t> [%l] [%s:%!:%#] %v");
//                    file_logger_->set_level(spdlog::level::debug);
//                    file_logger_->flush_on(spdlog::level::debug);
                    std::string log_level = getEnv("TANGO_LOG_LEVEL", "1");
                    file_logger_->set_level(static_cast<spdlog::level::level_enum>(std::stoi(log_level)));
                    file_logger_->flush_on(static_cast<spdlog::level::level_enum>(std::stoi(log_level)));
                }
                catch (const spdlog::spdlog_ex &ex) {
                    std::cerr << "Async Logger initialization failed: " << ex.what() << std::endl;
                }

                is_inited_.store(true);

                return true;
            }

        private:
            std::shared_ptr<spdlog::logger> file_logger_;

            std::atomic<bool> is_inited_;
        public:
            bool IsInit() const { return is_inited_.load(); }
        };

        // 运行时设置日志级别（同时作用于控制台与文件 sink）
        // 级别：0=trace 1=debug 2=info 3=warn 4=err 5=critical 6=off
        // 优先级：环境变量 TANGO_LOG_LEVEL > config.json Vwise.LogLevel > 默认 1
        inline void setLogLevel(int lvl) {
            if (lvl < 0) lvl = 0;
            if (lvl > 6) lvl = 6;
            auto lv = static_cast<spdlog::level::level_enum>(lvl);
            Logger<spdlog::logger>::getInstance().GetLogger()->set_level(lv);
            Logger<spdlog::logger>::getInstance().GetLogger()->flush_on(lv);
            Logger<spdlog::async_logger>::getInstance().GetLogger()->set_level(lv);
            Logger<spdlog::async_logger>::getInstance().GetLogger()->flush_on(lv);
        }

        // let logger like stream
        struct log_stream : public std::ostringstream {
        public:
            log_stream(const spdlog::source_loc &_loc, spdlog::level::level_enum _lvl)
                    : loc(_loc), lvl(_lvl) {
            }

            ~log_stream() {
                flush();
            }

            void flush() {
                Logger<spdlog::logger>::getInstance().GetLogger()->log(loc, lvl, str().c_str());

                if (Logger<spdlog::async_logger>::getInstance().IsInit()) {
                    Logger<spdlog::async_logger>::getInstance().GetLogger()->log(loc, lvl, str().c_str());
                } else {
                    Logger<spdlog::async_logger>::getInstance().init();
                }
            }

        private:
            spdlog::source_loc loc;
            spdlog::level::level_enum lvl = spdlog::level::info;
        };
    }  // namespace logger
}  // namespace cmsr
