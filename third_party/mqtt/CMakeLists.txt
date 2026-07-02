# Simplified CMakeLists.txt for paho-mqtt3a static library

cmake_minimum_required(VERSION 3.10.2)
project(paho-mqtt3a VERSION 1.3.13)

set(MQTT_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(MQTT_INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(MQTT_BIN_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Build timestamp
string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")

# Configure version info
configure_file(${MQTT_SRC_DIR}/VersionInfo.h.in
    ${MQTT_BIN_DIR}/VersionInfo.h
    @ONLY
)

# Common source files
set(common_src
    ${MQTT_SRC_DIR}/MQTTTime.c
    ${MQTT_SRC_DIR}/MQTTProtocolClient.c
    ${MQTT_SRC_DIR}/Clients.c
    ${MQTT_SRC_DIR}/utf-8.c
    ${MQTT_SRC_DIR}/MQTTPacket.c
    ${MQTT_SRC_DIR}/MQTTPacketOut.c
    ${MQTT_SRC_DIR}/Messages.c
    ${MQTT_SRC_DIR}/Tree.c
    ${MQTT_SRC_DIR}/Socket.c
    ${MQTT_SRC_DIR}/Log.c
    ${MQTT_SRC_DIR}/MQTTPersistence.c
    ${MQTT_SRC_DIR}/Thread.c
    ${MQTT_SRC_DIR}/MQTTProtocolOut.c
    ${MQTT_SRC_DIR}/MQTTPersistenceDefault.c
    ${MQTT_SRC_DIR}/SocketBuffer.c
    ${MQTT_SRC_DIR}/LinkedList.c
    ${MQTT_SRC_DIR}/MQTTProperties.c
    ${MQTT_SRC_DIR}/MQTTReasonCodes.c
    ${MQTT_SRC_DIR}/Base64.c
    ${MQTT_SRC_DIR}/SHA1.c
    ${MQTT_SRC_DIR}/WebSocket.c
    ${MQTT_SRC_DIR}/Proxy.c
    ${MQTT_SRC_DIR}/StackTrace.c
    ${MQTT_SRC_DIR}/Heap.c
    ${MQTT_SRC_DIR}/OsWrapper.c
)

# Build static library for paho-mqtt3a (async client)
add_library(paho-mqtt3a-static STATIC
    ${common_src}
    ${MQTT_SRC_DIR}/MQTTAsync.c
    ${MQTT_SRC_DIR}/MQTTAsyncUtils.c
)

target_include_directories(paho-mqtt3a-static
    PUBLIC
        ${MQTT_INC_DIR}
        ${MQTT_SRC_DIR}
    PRIVATE
        ${MQTT_BIN_DIR}
)

target_compile_definitions(paho-mqtt3a-static
    PRIVATE
        PAHO_MQTT_STATIC=1
        _GNU_SOURCE
)

target_link_libraries(paho-mqtt3a-static
    PUBLIC
        pthread
        dl
        c
        rt
)

set_target_properties(paho-mqtt3a-static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)