/**********************************************************************************************************************
    > File Name: global.h
    > Author: htsong
    > Date: 12/06/23
**********************************************************************************************************************/

#ifndef TANGO_NEXUS_COMMON_GLOBAL_H_
#define TANGO_NEXUS_COMMON_GLOBAL_H_

#include <string>

#include "macros.h"
#include <nlohmann/json.hpp>

namespace cmsr {
    namespace vwise {
        namespace common {
            using json = nlohmann::json;

            class GlobalData {
            public:
//                ~GlobalData();

            private:
                bool getJsonFromFile(const std::string &file_name, nlohmann::json& j); //file_name: with absolute path

            private:
                json json_;

                std::string imei_;
                std::mutex mutex_;

                json rwJson_;
                mutable std::mutex rwJson_mutex;
            public:
                nlohmann::json getRwJson() const;
                void setRwJson(const nlohmann::json& j);
                bool writeToFile(const nlohmann::json& j);

            public:
                const nlohmann::json &getJson() const;

                std::string getFileName(const std::string &path, const bool remove_extension = false);

                std::string programPath();

                std::string getImei();

                std::string acquireImeiFromAt();

                DECLARE_SINGLETON(GlobalData)
            };

        }  // namespace common
    }  // namespace vwise
}  // namespace cmsr

#endif  // TANGO_NEXUS_COMMON_GLOBAL_H_
