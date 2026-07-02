cmake_minimum_required(VERSION 3.8)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

include_directories(
        ${ROOT_DIR}/third_party/curl)

# 查找当前目录下的所有源文件，并将名称保存到 DIR_BASETOOLS_SRCS 变量中
aux_source_directory(. DIR_BASETOOLS_SRCS)

add_library(basetools ${DIR_BASETOOLS_SRCS})

# 使用系统库
find_package(CURL QUIET)
find_package(OpenSSL QUIET)
find_package(ZLIB QUIET)

target_link_libraries(basetools
        pthread
        dl
)

if(CURL_FOUND)
    target_link_libraries(basetools CURL::libcurl)
endif()

if(OpenSSL_FOUND)
    target_link_libraries(basetools OpenSSL::Crypto OpenSSL::SSL)
endif()

if(ZLIB_FOUND)
    target_link_libraries(basetools ZLIB::ZLIB)
endif()