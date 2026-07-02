cmake_minimum_required(VERSION 3.10.2)
project(v_wise_agent VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

set(ROOT_DIR ${PROJECT_SOURCE_DIR})

set(CMAKE_SKIP_RPATH TRUE)

# Find system libraries (optional, will warn if not found)
find_package(CURL QUIET)
find_package(OpenSSL QUIET)
find_package(spdlog QUIET)
find_package(nlohmann_json QUIET)

message(STATUS "Building for Raspberry Pi with system libraries")

# Check and report missing dependencies
if(NOT CURL_FOUND)
    message(WARNING "CURL not found - please install: sudo apt install libcurl4-openssl-dev")
endif()
if(NOT OpenSSL_FOUND)
    message(WARNING "OpenSSL not found - please install: sudo apt install libssl-dev")
endif()

include_directories(
        include
        basetools/include
        basetools
        common
        boards/rpi
        third_party/mqtt/include
        third_party/mqtt/src
)

# Use system spdlog if found, otherwise use third_party
if(NOT spdlog_FOUND)
    if(EXISTS "${ROOT_DIR}/third_party/spdlog/include")
        include_directories(third_party/spdlog/include)
        message(STATUS "Using third_party/spdlog")
    else()
        message(WARNING "spdlog not found - please install: sudo apt install libspdlog-dev")
    endif()
else()
    message(STATUS "Using system spdlog")
endif()

# Use system nlohmann_json if found, otherwise use third_party
if(NOT nlohmann_json_FOUND)
    if(EXISTS "${ROOT_DIR}/third_party/nlohmann_json")
        include_directories(third_party/nlohmann_json)
        message(STATUS "Using third_party/nlohmann_json")
    else()
        message(WARNING "nlohmann_json not found - please install: sudo apt install nlohmann-json3-dev")
    endif()
else()
    message(STATUS "Using system nlohmann_json")
endif()

add_subdirectory(basetools)
add_subdirectory(third_party/mqtt)

# Source files (using implementations from boards/rpi)
set(STUB_SRCS
    ${PROJECT_SOURCE_DIR}/boards/rpi/rpi_module_interface.cpp
    ${PROJECT_SOURCE_DIR}/boards/rpi/rpi_gps_info.cpp
    ${PROJECT_SOURCE_DIR}/boards/rpi/rpi_cell_info.cpp
    ${PROJECT_SOURCE_DIR}/boards/rpi/rpi_serial_port.cpp
    ${PROJECT_SOURCE_DIR}/boards/rpi/rpi_at_client.cpp
    ${PROJECT_SOURCE_DIR}/boards/rpi/rpi_urc.cpp
    ${PROJECT_SOURCE_DIR}/boards/rpi/rpi_satellite_terminal.cpp
)

aux_source_directory(${PROJECT_SOURCE_DIR}/src SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common COMMON_SRCS)

# Remove original files that were replaced by stubs
list(REMOVE_ITEM SRCS
    "${PROJECT_SOURCE_DIR}/src/zd_wrapper.cpp"
    "${PROJECT_SOURCE_DIR}/src/zd_gps_info.cpp"
    "${PROJECT_SOURCE_DIR}/src/cell_info.cpp"
    "${PROJECT_SOURCE_DIR}/src/minio.cpp"
)

add_executable(${PROJECT_NAME} ${SRCS} ${COMMON_SRCS} ${STUB_SRCS})

link_directories(${CMAKE_CURRENT_SOURCE_DIR}/output)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    basetools
    paho-mqtt3a-static
    pthread
    dl
    rt
)

# Link CURL if found
if(CURL_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)
    message(STATUS "Linking CURL")
else()
    message(STATUS "CURL not linked - some features may not work")
endif()

# Link OpenSSL if found
if(OpenSSL_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::Crypto OpenSSL::SSL)
    message(STATUS "Linking OpenSSL")
else()
    message(STATUS "OpenSSL not linked - some features may not work")
endif()

# Link spdlog if found
if(spdlog_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/data/config.json
    ${PROJECT_BINARY_DIR}/config.json
)