//
// Created by htsong on 2023/12/06.
//
#include <regex>
#include <iomanip>
#include <sys/timeb.h>
#include "util.h"
#include "log.h"
#include "base_timer.h"

#include <iostream>
#include <fstream>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

namespace base_tools {
    namespace util {
        using namespace std;

        struct FileInfo {
            string path;
            struct stat st;
        };

        void creatFilePath(std::string path) {
            std::string folderName = path; // 文件夹的名称
            int status = mkdir(folderName.c_str(), S_IRUSR | S_IWUSR | S_IXUSR | S_IRWXG | S_IRWXO);
            if (status == 0) {
                LogInfo << "creat mkdir path success ";
            } else {
                LogInfo << "creat mkdir path failed" ;
            }
        }

        void writeLogToFile(std::string file, std::string logStr) {
            std::ofstream outputFile(file, std::ios::app); //如果文件存在，继续往下写
        
            if (outputFile.is_open()) { // 检查文件是否成功打开
                outputFile << logStr << std::endl;
                // 关闭文件
                outputFile.close();

            } else {
                LogInfo << "Can not open writeLogToFile ..." ;
            }     
        }
  
        off_t get_directory_size(const string &path) {
            DIR *dir = opendir(path.c_str());
            off_t size = 0;
            struct dirent *entry;
        
            while ((entry = readdir(dir))) {
                if (entry->d_type == DT_REG) { // 常规文件
                    string full_path = path + "/" + entry->d_name;
                    struct stat st;
                    if (stat(full_path.c_str(), &st) == 0) {
                        size += st.st_size;
                    }
                }
            }
            closedir(dir);
            return size;
        }
        
        // 比较两个stat结构体的时间，返回较早的一个
        struct stat older(const struct stat &a, const struct stat &b) {
            return a.st_ctime < b.st_ctime ? a : b;
        }
        
        // 删除最老的文件
        void removeOldestFile(const std::string &path) {
            struct stat oldest = {0};
            std::string oldestFile;
            DIR *dir = opendir(path.c_str());
            struct dirent *entry;
        
            while ((entry = readdir(dir))) {
                if (entry->d_type == DT_REG) { // 只考虑普通文件
                    std::string filePath = path + "/" + entry->d_name;
                    struct stat info;
                    if (stat(filePath.c_str(), &info) == 0) {
                        if (oldest.st_ctime == 0 || older(info, oldest).st_ctime > oldest.st_ctime) {
                            oldest = info;
                            oldestFile = filePath;
                        }
                    }
                }
            }
            closedir(dir);
            LogInfo<<"remove oldestFile: "<< oldestFile;
            if (!oldestFile.empty()) {
                if (std::remove(oldestFile.c_str()) != 0) {
                    LogError<< "Error deleting file: " << oldestFile;
                }
            }
        }

        void folderSizeCheck(std::string path) {
            off_t directory_size = get_directory_size(path);
            LogInfo<< "Current directory size: " << directory_size/1024<< " KB";
        
            // 假设我们要保持目录小于10MB
            const off_t MAX_SIZE = 10 * 1024 * 1024; 
            if (directory_size >= MAX_SIZE) {
                removeOldestFile(path);
                //directory_size = get_directory_size(path);
            }
        }

        std::vector<std::string> split(const string &input, const string &delimiters) {
            // passing -1 as the submatch index parameter performs splitting
            std::regex re(delimiters);
            std::sregex_token_iterator
                    first{input.begin(), input.end(), re, -1},
                    last;
            return {first, last};
        }

        std::vector<std::string> split_multi(const std::string &str, char delimiter) {
            std::vector<std::string> tokens;
            std::string token;
            std::istringstream tokenStream(str);
            while (std::getline(tokenStream, token, delimiter)) {
                if (!token.empty()) {
                    tokens.push_back(token);
                }
            }
            return tokens;
        }

        std::string char2hexstr(char *str, int n) {
            std::ostringstream oss;
            oss << std::hex;
            oss << std::setfill('0');
            oss << std::uppercase;   //大写
            for (int i = 0; i < n; i++) {
                unsigned char c = str[i];
//        oss  << "0x" << std::setw(2) << (unsigned int)c;
//        if (i < n - 1)
//            oss << ',';
                oss << std::setw(2) << (unsigned int) c;
            }
            return oss.str();
        }


        int getSecMark() {
            struct tm *ptm;
            struct timeb stTimeb;
            static char tmp[64];

            ftime(&stTimeb);
            ptm = localtime(&stTimeb.time);
            strftime(tmp, sizeof(tmp), "%Y-%m-%d %H:%M:%S", ptm);
            sprintf(tmp, "%s.%03d", tmp, stTimeb.millitm);
            string nowtime(tmp);
            string sRet = nowtime.substr(nowtime.size() - 6);
            try {
                return std::stod(sRet) * 1000;
            }
            catch (const std::invalid_argument &e) {
                LogError << "invalid_argument: " << e.what() << std::endl;
            }
            catch (const std::out_of_range &e) {
                LogError << "out_of_range: " << e.what() << std::endl;
            }
            catch (...) {
                LogError << "exception!!! " << std::endl;
            }
            return 60000;
        }

        string len8_id() {
            long long nowTime = BaseTimer::GetMilliTime();
            string strNow = std::to_string(nowTime);
            return strNow.substr(strNow.size() - 8);
        }

        std::string strip(const std::string &s) {
            int i = 0;
            while (i < s.size() && isspace(s[i])) i++;
            int j = s.size() - 1;
            while (j >= 0 && isspace(s[j])) j--;  // while (j >= 0 && (isspace(s[j]) || s[j] == ',')) j--;
            return s.substr(i, j - i + 1);
        }

        std::string sysTimeMqtt(void)
        {
            #ifdef WIN32
            SYSTEMTIME st = { 0 };
                GetLocalTime(&st);  //获取当前时间 可精确到ms
                std::ostringstream ss;
                ss << std::setfill('0') << std::setw(4) << st.wYear
                    << "-"
                    << std::setw(2) << st.wMonth
                    << "-"
                    << std::setw(2) << st.wDay
                    << " "
                    << std::setw(2) << st.wHour
                    << ":"
                    << std::setw(2) << st.wMinute
                    << ":"
                    << std::setw(2) << st.wSecond
                    << ":"
                    << std::setw(3) << st.wMilliseconds;

                return ss.str();	
            #else
            struct timeval tv;
            struct tm* ptm;
            char time_string[40];
            long milliseconds;
            char buff[50]; 
            gettimeofday(&tv, NULL);
            ptm = localtime (&(tv.tv_sec));
            strftime (time_string, sizeof(time_string), "%Y-%m-%d %H:%M:%S", ptm);  //输出格式为: 2022-03-30 20:38:37
            milliseconds = tv.tv_usec / 1000;
            snprintf (buff, 50, "%s.%03ld", time_string, milliseconds);
            std::string ss = buff;
            return ss;  
            #endif

        }


        std::string sysTimeLog(void)
        {
            #ifdef WIN32
            SYSTEMTIME st = { 0 };
                GetLocalTime(&st);  //获取当前时间 可精确到ms
                std::ostringstream ss;
                ss << std::setfill('0') << std::setw(4) << st.wYear
                   << std::setw(2) << st.wMonth
                   << std::setw(2) << st.wDay
       
                return ss.str();	
            #else
            struct timeval tv;
            struct tm* ptm;
            char time_string[40];
            long milliseconds;
            char buff[50]; 
            gettimeofday(&tv, NULL);
            ptm = localtime (&(tv.tv_sec));
            strftime (time_string, sizeof(time_string), "%Y%m%d", ptm);  //输出格式为: 20220330 
            snprintf (buff, 50, "%s", time_string);
            std::string ss = buff;
            return ss;  
            #endif

        }

        bool fileExistsInFolder(const std::string& folderPath, const std::string& fileName) 
        {
            DIR* dir;
            struct dirent *entry;
            
            if ((dir = opendir(folderPath.c_str())) != nullptr) {
                while ((entry = readdir(dir)) != nullptr) {
                    if (fileName == entry->d_name) {
                        closedir(dir);
                        return true;
                    }
                }
                
                closedir(dir);
            } else {
                perror("Failed to open directory");
                exit(EXIT_FAILURE);
            }
            
            return false;
        }

        bool isEqualToZero(double value, double epsilon) {
            return std::fabs(value) < epsilon;
        }
        
        double transform_latitude(double lon, double lat) {
            double ret = -100.0 + 2.0 * lon + 3.0 * lat + 0.2 * lat * lat + 0.1 * lon * lat + 0.2 * sqrt(fabs(lon));
            ret += (20.0 * sin(6.0 * lon * M_PI) + 20.0 * sin(2.0 * lon * M_PI)) * 2.0 / 3.0;
            ret += (20.0 * sin(lat * M_PI) + 40.0 * sin(lat / 3.0 * M_PI)) * 2.0 / 3.0;
            ret += (160.0 * sin(lat / 12.0 * M_PI) + 320 * sin(lat * M_PI / 30.0)) * 2.0 / 3.0;
            return ret;
        }

        double transform_longitude(double lon, double lat) {
            double ret = 300.0 + lon + 2.0 * lat + 0.1 * lon * lon + 0.1 * lon * lat + 0.1 * sqrt(fabs(lon));
            ret += (20.0 * sin(6.0 * lon * M_PI) + 20.0 * sin(2.0 * lon * M_PI)) * 2.0 / 3.0;
            ret += (20.0 * sin(lon * M_PI) + 40.0 * sin(lon / 3.0 * M_PI)) * 2.0 / 3.0;
            ret += (150.0 * sin(lon / 12.0 * M_PI) + 300.0 * sin(lon / 30.0 * M_PI)) * 2.0 / 3.0;
            return ret;
        }

#define A 6378245.0
#define EE 0.00669342162296594323
        void wgs84ToGcj02(double lon, double lat, double &gcjlon, double &gcjlat) {

            double d_lat = transform_latitude(lon - 105.0, lat - 35.0);
            double d_lon = transform_longitude(lon - 105.0, lat - 35.0);
            double rad_lat = lat / 180.0 * M_PI;
            double magic = sin(rad_lat);
            magic = 1 - EE * magic * magic;
            double sqrt_magic = sqrt(magic);
            d_lat = (d_lat * 180.0) / ((A * (1 - EE)) / (magic * sqrt_magic) * M_PI);
            d_lon = (d_lon * 180.0) / (A / sqrt_magic * cos(rad_lat) * M_PI);
            gcjlon = lon + d_lon;
            gcjlat = lat + d_lat;
        }

    } // namespace util
} // namespace base_tools
