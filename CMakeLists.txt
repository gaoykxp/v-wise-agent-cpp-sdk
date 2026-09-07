cmake_minimum_required(VERSION 3.10.2)
project(v_wise_agent VERSION 1.0.1)

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
        src/modem
        third_party/mqtt/include
        third_party/mqtt/src
)

# ==================== 模组驱动选择（编译期绑定） ====================
# 方案见《SDK多模组兼容设计-编译选项方案.md》：
#   CMake 选项 VWISE_MODEM → 编译定义 VWISE_MODEM_DRIVER_* → src/modem/modem_registry.cpp
#   选择方言层驱动；业务层一律经 vwise::modem::modemDriver() 访问。
# 新增模组：drivers/<vendor>/<model>/ 实现驱动 → registry 加 #elif → 此处加映射。
set(VWISE_MODEM "quectel_5g" CACHE STRING "蜂窝模组驱动（方言层）：quectel_5g | stub")
message(STATUS "VWISE_MODEM = ${VWISE_MODEM}")

set(MODEM_DRIVER_SRCS "")
if(VWISE_MODEM STREQUAL "quectel_5g")
    set(VWISE_MODEM_DRIVER_DEF VWISE_MODEM_DRIVER_QUECTEL_5G)
    list(APPEND MODEM_DRIVER_SRCS
            ${PROJECT_SOURCE_DIR}/src/modem/drivers/quectel_5g/quectel_5g_driver.cpp)
elseif(VWISE_MODEM STREQUAL "stub")
    # 无模组环境/单元测试：空驱动（全部能力缺失，语义层自然降级）
    set(VWISE_MODEM_DRIVER_DEF VWISE_MODEM_STUB)
else()
    message(FATAL_ERROR "未知模组驱动 VWISE_MODEM=${VWISE_MODEM}（支持：quectel_5g | stub）")
endif()
add_definitions(-D${VWISE_MODEM_DRIVER_DEF})

# [临时测试] 模组软重启验证开关（probe_mgr.cpp 尾部 AT+CFUN=0/1 循环），默认关闭。
# 打开：cmake -DVWISE_TEST_MODULE_RESET=ON（build_rpi.sh 环境变量同名透传）
option(VWISE_TEST_MODULE_RESET "模组软重启验证临时代码（默认关闭）" OFF)
if(VWISE_TEST_MODULE_RESET)
    add_definitions(-DVWISE_TEST_MODULE_RESET=1)
endif()

# 从 project VERSION 生成版本号头文件（版本号单一来源：上面的 project(... VERSION x.y.z)）
configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/include/version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/include/version.h
        @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/include)

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

# 模组方言层（aux_source_directory 非递归，src/modem 单独收录）
set(MODEM_SRCS
        ${PROJECT_SOURCE_DIR}/src/modem/diag_event_bus.cpp
        ${PROJECT_SOURCE_DIR}/src/modem/modem_registry.cpp
        ${MODEM_DRIVER_SRCS}
)

# Remove original files that were replaced by stubs
list(REMOVE_ITEM SRCS
    "${PROJECT_SOURCE_DIR}/src/zd_wrapper.cpp"
    "${PROJECT_SOURCE_DIR}/src/zd_gps_info.cpp"
    "${PROJECT_SOURCE_DIR}/src/cell_info.cpp"
    "${PROJECT_SOURCE_DIR}/src/minio.cpp"
)

add_executable(${PROJECT_NAME} ${SRCS} ${COMMON_SRCS} ${MODEM_SRCS} ${STUB_SRCS})

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
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/conf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/data/config.json
    ${PROJECT_BINARY_DIR}/conf/config.json
)