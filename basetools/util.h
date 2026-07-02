//
// Created by htsong on 2023/12/06.
//
#ifndef BASE_TOOLS_UTIL_H_
#define BASE_TOOLS_UTIL_H_

#include <string>
#include <vector>

namespace base_tools {
    namespace util {

        void creatFilePath(std::string path);

        void writeLogToFile(std::string file, std::string logStr);

        void folderSizeCheck(std::string path);
        std::string strip(const std::string &s);

        std::vector<std::string> split(const std::string &input, const std::string &delimiters);
        std::vector<std::string> split_multi(const std::string &str, char delimiter);

        std::string char2hexstr(char *str, int n);

        int getSecMark();

        //生成uuid
        std::string len8_id();
        std::string sysTimeMqtt(void);
        std::string sysTimeLog(void);
        bool fileExistsInFolder(const std::string& folderPath, const std::string& fileName);

        bool isEqualToZero(double value, double epsilon = 1e-9);

        void wgs84ToGcj02(double lon, double lat, double &gcjlon, double &gcjlat);

    } // namespace util
} // namespace base_tools


#endif  // BASE_TOOLS_UTIL_H_
