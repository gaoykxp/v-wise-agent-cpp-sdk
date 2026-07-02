/**********************************************************************************************************************
    > File Name: basezlib.cpp
    > Author: htsong
    > Date: 7/1/24
**********************************************************************************************************************/
#ifndef BASE_TOOLS_BASE_ZLIB_H
#define BASE_TOOLS_BASE_ZLIB_H

#include <string>
#include <vector>

namespace base_tools
{

class BaseZlib{
public:
    static bool compressFile(const std::string& inputFilePath, const std::string& outputFilePath);
    static bool decompressFile(const std::string& inputFilePath, const std::string& outputFilePath);
    std::vector<std::string> getFilesInDirectory(const std::string& directoryPath);
    void traverseFolder(const std::string& folderPath);

};

}
#endif  // BASE_TOOLS_BASE_ZLIB_H
