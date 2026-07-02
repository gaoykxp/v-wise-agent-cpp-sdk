/**********************************************************************************************************************
    > File Name: basezlib.cpp
    > Author: htsong
    > Date: 7/1/24
**********************************************************************************************************************/

#include "basezlib.h"
#include "log.h"

#include <iostream>
#include <fstream>
#include <zlib.h>
#include <dirent.h>
#include <string.h>

//#include <filesystem>   // need C++ 17

namespace base_tools{

    bool BaseZlib::compressFile(const std::string& inputFilePath, const std::string& outputFilePath) {
        std::ifstream inputFile(inputFilePath, std::ios::binary);
        if (!inputFile) {
            LogError << "Can not open inputfile:" << inputFilePath << std::endl;
            return false;
        }

        std::ofstream outputFile(outputFilePath, std::ios::binary);
        if (!outputFile) {
            LogError << "Can not open outputfile:" << outputFilePath << std::endl;
            return false;
        }

        std::vector<char> buffer(1024 * 1024);
        z_stream strm;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        int ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
        if (ret != Z_OK) {
            LogError << "deflateInit2 failed: " << ret << std::endl;
            return false;
        }

        strm.avail_in = inputFile.readsome(buffer.data(), buffer.size());
        do {
            strm.next_in = reinterpret_cast<Bytef*>(buffer.data());
            strm.avail_out = buffer.size();
            strm.next_out = reinterpret_cast<Bytef*>(buffer.data());

            ret = deflate(&strm, Z_FINISH);
            if (ret == Z_STREAM_ERROR) {
                LogError << "deflate failed: " << ret << std::endl;
                deflateEnd(&strm);
                return false;
            }

            outputFile.write(buffer.data(), buffer.size() - strm.avail_out);
            strm.avail_in = inputFile.readsome(buffer.data(), buffer.size());
        } while (strm.avail_in > 0);

        deflateEnd(&strm);
        inputFile.close();
        outputFile.close();
        return true;
    }

    bool BaseZlib::decompressFile(const std::string& inputFilePath, const std::string& outputFilePath) {
        std::ifstream inputFile(inputFilePath, std::ios::binary);
        if (!inputFile) {
            LogError << "Can not open inputfile: " << inputFilePath << std::endl;
            return false;
        }

        std::ofstream outputFile(outputFilePath, std::ios::binary);
        if (!outputFile) {
            LogError << "Can not open outputfile: " << outputFilePath << std::endl;
            return false;
        }

        std::vector<char> buffer(1024 * 1024);
        z_stream strm;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = 0;
        strm.next_in = Z_NULL;
        int ret = inflateInit2(&strm, 31);
        if (ret != Z_OK) {
            LogError << "inflateInit2 failed: " << ret << std::endl;
            return false;
        }

        do {
            inputFile.readsome(buffer.data(), buffer.size());
            strm.avail_in = static_cast<uInt>(inputFile.gcount());
            if (strm.avail_in == 0) {
                break;
            }
            strm.next_in = reinterpret_cast<Bytef*>(buffer.data());

            do {
                strm.avail_out = buffer.size();
                strm.next_out = reinterpret_cast<Bytef*>(buffer.data());
                ret = inflate(&strm, Z_NO_FLUSH);
                if (ret == Z_STREAM_ERROR) {
                    LogError << "inflate failed:  " << ret << std::endl;
                    inflateEnd(&strm);
                    return false;
                }
                outputFile.write(buffer.data(), buffer.size() - strm.avail_out);
            } while (strm.avail_out == 0);
        } while (!inputFile.eof());

        inflateEnd(&strm);
        inputFile.close();
        outputFile.close();
        return true;
    }

    std::vector<std::string> BaseZlib::getFilesInDirectory(const std::string& directoryPath) {
        std::vector<std::string> files;
        DIR* dir = opendir(directoryPath.c_str());
        if (dir == nullptr) {
            return files;
        }
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_REG) {
                files.push_back(entry->d_name);
            }
        }
        closedir(dir);
        return files;
    }

    void BaseZlib::traverseFolder(const std::string& folderPath) {
        std::vector<std::string> files = getFilesInDirectory(folderPath);
        for (const auto& file : files) {
            LogInfo << "File: " << file << std::endl;
        }
        std::vector<std::string> directories;
        DIR* dir = opendir(folderPath.c_str());
        if (dir == nullptr) {
            return;
        }
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                directories.push_back(entry->d_name);
            }
        }
        closedir(dir);
        for (const auto& directory : directories) {
            LogInfo << "Directory: " << directory << std::endl;
            traverseFolder(folderPath + "/" + directory); // 递归遍历子文件夹
        }
    }

/*
// 需要 C++ 17
void compressFolder(const std::string& inputFolderPath, const std::string& outputFolderPath) {
    for (const auto& entry : std::filesystem::directory_iterator(inputFolderPath)) {
        if (entry.is_regular_file()) {
            std::string inputFilePath = entry.path().string();
            std::string outputFilePath = outputFolderPath + "/" + entry.path().filename().string() + ".zip";
            if (!compressFile(inputFilePath, outputFilePath)) {
                std::cerr << "压缩文件失败： " << inputFilePath << std::endl;
            }
        }
    }
}


// 需要 C++ 17
void decompressFolder(const std::string& inputFolderPath, const std::string& outputFolderPath) {
    for (const auto& entry : std::filesystem::directory_iterator(inputFolderPath)) {
        if (entry.is_regular_file()) {
            std::string inputFilePath = entry.path().string();
            std::string outputFilePath = outputFolderPath + "/" + entry.path().filename().string();
            if (!decompressFile(inputFilePath, outputFilePath)) {
                std::cerr << "解压文件失败： " << inputFilePath << std::endl;
            }
        }
    }
}
*/


} //namespace base_tools
