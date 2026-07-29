/**********************************************************************************************************************
    > File Name: global.cpp
    > Author: htsong
    > Date: 12/06/23
**********************************************************************************************************************/

#include <climits>
#include <fstream>
#include "unistd.h"
#include "global.h"
#include "log.h"
#include "util.h"

namespace {
    const std::string EMPTY_STRING = "";
    const std::string CONFIG_NAME = "/conf/config.json";
    const std::string RW_CONFIG_NAME = "/conf/rw.conf";
}  // namespace

namespace cmsr {
    namespace vwise {
        namespace common {

            GlobalData::GlobalData() {
                auto config_path = programPath(); // exe_dir, e.g. /system/cmsr/vobu
                auto file_name = getFileName(config_path); // e.g. vobu
                config_path = config_path.substr(0, config_path.size() - file_name.size()); //remove fileName from programPath
                std::string confName{config_path};
                confName.append(CONFIG_NAME);
                std::string rwConfName{config_path};
                rwConfName.append(RW_CONFIG_NAME);
                LogDebug << "config_path: " << config_path << ", confName: " << confName << ", rwConfName: " << rwConfName;
                if (!getJsonFromFile(confName, json_)) {
                    LogError << "read conf file failed!";
                }
                if (!getJsonFromFile(rwConfName, rwJson_)) {
                    LogError << "read rw.conf file failed!";
                }
                //SDK_Init();
                //net_para oPara;
                //SDK_Get_Para(&oPara);
                //imei_ = oPara.imei;
                // char imsi[16] = {0};
                // int ret = zd_sim_get_imsi(0, imsi);
                // LogInfo<<"get imei:"<< imsi;

                // imei_ = imsi;

                if(imei_.size()==0)
                {
		            imei_ = json_["Vwise"].value("Imei", "");
		        }
                //SDK_DeInit();
                

//                LogDebug << "rwJson: " << rwJson_.dump() << ", global json: " << json_.dump();
            }

            bool GlobalData::getJsonFromFile(const std::string &file_name, nlohmann::json& j) {
                std::ifstream ifs(file_name);
//                json_ = json::parse(ifs);

                if (!ifs.is_open()) {
                    LogError << "Failed to open file " << file_name;
                    return false;
                }
                ifs >> j;
                ifs.close();

                return true;
            }

            const json &GlobalData::getJson() const {
                return json_;
            }

            nlohmann::json GlobalData::getRwJson() const {
                std::lock_guard<std::mutex> lock(rwJson_mutex);
                return rwJson_;
            }

            void GlobalData::setRwJson(const nlohmann::json& j){
                std::lock_guard<std::mutex> lock(rwJson_mutex);
                rwJson_ = j;
            }

            bool GlobalData::writeToFile(const nlohmann::json& j){
                std::ofstream outputFile("/etc/rw.conf");
                if (!outputFile.is_open()) {
                    LogError << "Failed to open file rw.conf";
                    return false;
                }

                outputFile << j.dump(4); // 使用4个空格缩进格式化输出
                outputFile.close();
                return true;
            }

            std::string GlobalData::programPath() {
                char path[PATH_MAX] = {0};
                auto len = readlink("/proc/self/exe", path, sizeof(path) - 1);
                if (len == -1) {
                    return EMPTY_STRING;
                }
                path[len] = '\0';
                return std::string(path);
            }

            std::string GlobalData::getFileName(const std::string &path, const bool remove_extension) {
                std::string::size_type start = path.rfind('/');
                if (start == std::string::npos) {
                    start = 0;
                } else {
                    // Move to the next char after '/'.
                    ++start;
                }

                std::string::size_type end = std::string::npos;
                if (remove_extension) {
                    end = path.rfind('.');
                    // The last '.' is found before last '/', ignore.
                    if (end != std::string::npos && end < start) {
                        end = std::string::npos;
                    }
                }
                const auto len = (end != std::string::npos) ? end - start : end;
                return path.substr(start, len);
            }

            std::string GlobalData::getImei(){
                std::lock_guard<std::mutex> lg(mutex_);
                return imei_;
            }

            std::string GlobalData::acquireImeiFromAt(){
                const std::string AT_CMD = "\"AT+CGSN?\"";
                std::ostringstream commandStream;
                commandStream << "atcmd /dev/ttyUSB1 9600 5 " << AT_CMD << " > imei.info";
                int ret = std::system(commandStream.str().c_str());
                if( 0 != ret){
                    LogError << "system call ret: " << ret;
                    return EMPTY_STRING;
                }
                usleep(50 * 1000);

                std::ifstream infile("imei.info");
                if (!infile.good()) {
                    LogError << "read file  imei.info error!!!";
                    return EMPTY_STRING;
                }

                std::string message;
                while (getline(infile, message)) {
                    LogInfo << "read file, " << message;
                    if(message.find("+CGSN:") != std::string::npos){
                        break;
                    }
                }
                std::vector<std::string> results = base_tools::util::split(message, "\"");
                imei_ = results[1];
                return imei_;
            }

        }  // namespace common
    }  // namespace vwise
}  // namespace cmsr
