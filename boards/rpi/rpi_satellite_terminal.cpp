/**********************************************************************************************************************
    > File Name: rpi_satellite_terminal.cpp
    > Author: Satellite Terminal Protocol Implementation
    > Date: 05/09/26
    > Description: Implementation of satellite terminal communication

    Protocol format (NMEA-like):
    $<type>,<field1>,<field2>,...*<checksum><CR><LF>

    IC Query:
    Send: $CCGMO,ICI,2,0*28\r\n
    Receive: $GSICI,<type>,<status>,<signal>,<strength>,<lat>,<lon>,<alt>,<battery>,<power>,<work>*xx\r\n

    Version Query:
    Send: $CCGMO,DII,2,0*2F\r\n
    Receive: $GSDII,<type>,<hw1>,<hw2>,<sw_ver>,<module_ver>,<date>,<status>,<error>,<lat>,<lat_dir>,<lon>,<lon_dir>*xx\r\n

    Signal Query:
    Send: $CCGMO,PDI,2,1*37\r\n
    Receive: $GSPDI,<time>,<mode>,<signal>,<f3>,<f4>,<f5>,<f6>,<quality>*xx\r\n
    Example: $GSPDI,001838,1,158,0,0,1,1,45*72\r\n
**********************************************************************************************************************/
#include "rpi_satellite_terminal.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <iostream>
#include "log.h"

namespace rpi {

// ============== Constructor/Destructor ==============

SatelliteTerminal::SatelliteTerminal() = default;

SatelliteTerminal::~SatelliteTerminal() {
    close();
}

// ============== Connection Management ==============

bool SatelliteTerminal::open(const SatelliteTerminalConfig& config) {
    if (port_.is_open()) {
        return true;
    }

    config_ = config;

    if (!port_.open(config.port_name, config.baud_rate)) {
        LogError << "Failed to open satellite terminal port: " << config.port_name;
        return false;
    }

    connected_.store(true);
    LogInfo << "Satellite terminal opened on " << config.port_name
            << " at " << config.baud_rate << " baud";
    return true;
}

void SatelliteTerminal::close() {
    if (!connected_.exchange(false)) {
        return;
    }

    if (port_.is_open()) {
        port_.close();
    }

    LogInfo << "Satellite terminal closed";
}

bool SatelliteTerminal::is_open() const {
    return port_.is_open();
}

// ============== Query Operations ==============

std::optional<IcInfo> SatelliteTerminal::query_ic_info(int timeout_ms) {
    if (!port_.is_open()) {
        LogError << "Satellite terminal not connected";
        return std::nullopt;
    }

    // Build and send IC query statement
    // Format: $CCGMO,ICI,2,0*28\r\n
    std::vector<std::string> fields = {"ICI", "2", "0"};
    std::string statement = build_statement("CCGMO", fields);

    LogDebug << "Sending IC query: " << statement;

    if (!send_statement(statement)) {
        LogError << "Failed to send IC query statement";
        return std::nullopt;
    }

    // Wait for GSICI response
    auto response = wait_for_response("GSICI", timeout_ms);
    if (!response) {
        LogWarn << "No GSICI response received within timeout";
        return std::nullopt;
    }

    LogDebug << "Received GSICI response: " << *response;

    // Parse response
    std::string type;
    std::vector<std::string> resp_fields;
    if (!parse_statement(*response, type, resp_fields)) {
        LogError << "Failed to parse GSICI statement or checksum invalid";
        return std::nullopt;
    }

    return parse_gsici(resp_fields, *response);
}

std::optional<DeviceInfo> SatelliteTerminal::query_device_info(int timeout_ms) {
    if (!port_.is_open()) {
        LogError << "Satellite terminal not connected";
        return std::nullopt;
    }

    // Build and send version query statement
    // Format: $CCGMO,DII,2,0*2F\r\n
    std::vector<std::string> fields = {"DII", "2", "0"};
    std::string statement = build_statement("CCGMO", fields);

    LogDebug << "Sending DII query: " << statement;

    if (!send_statement(statement)) {
        LogError << "Failed to send DII query statement";
        return std::nullopt;
    }

    // Wait for GSDII response
    auto response = wait_for_response("GSDII", timeout_ms);
    if (!response) {
        LogWarn << "No GSDII response received within timeout";
        return std::nullopt;
    }

    LogDebug << "Received GSDII response: " << *response;

    // Parse response
    std::string type;
    std::vector<std::string> resp_fields;
    if (!parse_statement(*response, type, resp_fields)) {
        LogError << "Failed to parse GSDII statement or checksum invalid";
        return std::nullopt;
    }

    return parse_gsdii(resp_fields, *response);
}

std::optional<SignalInfo> SatelliteTerminal::query_signal_info(int timeout_ms) {
    if (!port_.is_open()) {
        LogError << "Satellite terminal not connected";
        return std::nullopt;
    }

    // Build and send signal query statement
    // Format: $CCGMO,PDI,2,1*37\r\n
    std::vector<std::string> fields = {"PDI", "2", "1"};
    std::string statement = build_statement("CCGMO", fields);

    LogDebug << "Sending PDI query: " << statement;

    if (!send_statement(statement)) {
        LogError << "Failed to send PDI query statement";
        return std::nullopt;
    }

    // Wait for GSPDI response
    auto response = wait_for_response("GSPDI", timeout_ms);
    if (!response) {
        LogWarn << "No GSPDI response received within timeout";
        return std::nullopt;
    }

    LogDebug << "Received GSPDI response: " << *response;

    // Parse response
    std::string type;
    std::vector<std::string> resp_fields;
    if (!parse_statement(*response, type, resp_fields)) {
        LogError << "Failed to parse GSPDI statement or checksum invalid";
        return std::nullopt;
    }

    return parse_gspdi(resp_fields, *response);
}

// ============== Raw Statement Operations ==============

bool SatelliteTerminal::send_statement(const std::string& statement) {
    if (!port_.is_open()) {
        return false;
    }

    // Ensure statement has proper terminators
    std::string to_send = statement;
    if (to_send.find("\r\n") == std::string::npos) {
        to_send += "\r\n";
    }

    return port_.write(to_send);
}

std::optional<std::string> SatelliteTerminal::read_statement(int timeout_ms) {
    if (!port_.is_open()) {
        return std::nullopt;
    }

    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        auto chunk = port_.read_some(std::chrono::milliseconds(200));
        if (!chunk.empty()) {
            buffer += chunk;
        }

        // Look for complete statement (ends with \r\n)
        size_t crlf_pos = buffer.find("\r\n");
        if (crlf_pos != std::string::npos) {
            std::string statement = buffer.substr(0, crlf_pos + 2);
            return statement;
        }

        // Also check for \n only (some terminals may use single LF)
        size_t lf_pos = buffer.find('\n');
        if (lf_pos != std::string::npos) {
            std::string statement = buffer.substr(0, lf_pos + 1);
            return statement;
        }
    }

    // Timeout - return what we have if it looks like a statement
    if (!buffer.empty() && buffer.find('$') != std::string::npos) {
        return buffer;
    }

    return std::nullopt;
}

// ============== Callbacks ==============

void SatelliteTerminal::set_ic_info_callback(IcInfoCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    ic_info_callback_ = std::move(callback);
}

void SatelliteTerminal::set_device_info_callback(DeviceInfoCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    device_info_callback_ = std::move(callback);
}

void SatelliteTerminal::set_signal_info_callback(SignalInfoCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    signal_info_callback_ = std::move(callback);
}

// ============== Protocol Utilities ==============

std::string SatelliteTerminal::calculate_checksum(const std::string& content) {
    // XOR of all characters
    uint8_t checksum = 0;
    for (char c : content) {
        checksum ^= static_cast<uint8_t>(c);
    }

    // Convert to 2-char hex string (uppercase)
    char hex[3];
    snprintf(hex, sizeof(hex), "%02X", checksum);
    return std::string(hex);
}

bool SatelliteTerminal::verify_checksum(const std::string& statement) {
    // Find $ and * positions
    size_t dollar_pos = statement.find('$');
    size_t star_pos = statement.find('*');

    if (dollar_pos == std::string::npos || star_pos == std::string::npos) {
        return false;
    }

    if (star_pos <= dollar_pos) {
        return false;
    }

    // Extract content between $ and * (exclusive)
    std::string content = statement.substr(dollar_pos + 1, star_pos - dollar_pos - 1);

    // Extract checksum after *
    std::string checksum_str;
    size_t checksum_start = star_pos + 1;
    size_t checksum_end = statement.find('\r', checksum_start);
    if (checksum_end == std::string::npos) {
        checksum_end = statement.find('\n', checksum_start);
    }
    if (checksum_end == std::string::npos) {
        checksum_end = statement.length();
    }
    checksum_str = statement.substr(checksum_start, checksum_end - checksum_start);

    // Remove any trailing whitespace from checksum
    checksum_str = trim(checksum_str);

    // Calculate expected checksum
    std::string expected_checksum = calculate_checksum(content);

    // Compare (case-insensitive for hex)
    if (checksum_str.length() != 2) {
        return false;
    }

    return (checksum_str[0] == expected_checksum[0] ||
            std::toupper(checksum_str[0]) == expected_checksum[0]) &&
           (checksum_str[1] == expected_checksum[1] ||
            std::toupper(checksum_str[1]) == expected_checksum[1]);
}

std::string SatelliteTerminal::build_statement(const std::string& type,
                                                 const std::vector<std::string>& fields) {
    // Build: $<type>,<field1>,<field2>,...*<checksum>\r\n
    std::ostringstream oss;
    oss << "$" << type;

    for (const auto& field : fields) {
        oss << "," << field;
    }

    // Content for checksum: everything after $, before *
    std::string content = oss.str().substr(1);  // Skip the $

    std::string checksum = calculate_checksum(content);
    oss << "*" << checksum << "\r\n";

    return oss.str();
}

bool SatelliteTerminal::parse_statement(const std::string& statement,
                                         std::string& type,
                                         std::vector<std::string>& fields) {
    // First verify checksum
    if (!verify_checksum(statement)) {
        LogWarn << "Checksum verification failed for: " << statement;
        return false;
    }

    // Find $ and * positions
    size_t dollar_pos = statement.find('$');
    size_t star_pos = statement.find('*');

    if (dollar_pos == std::string::npos || star_pos == std::string::npos) {
        return false;
    }

    // Extract content between $ and *
    std::string content = statement.substr(dollar_pos + 1, star_pos - dollar_pos - 1);

    // Split by comma
    fields.clear();
    std::istringstream iss(content);
    std::string token;

    // First token is the type
    if (std::getline(iss, token, ',')) {
        type = trim(token);
    } else {
        return false;
    }

    // Remaining tokens are fields
    while (std::getline(iss, token, ',')) {
        fields.push_back(trim(token));
    }

    return true;
}

// ============== Private Methods ==============

std::optional<std::string> SatelliteTerminal::wait_for_response(const std::string& expected_type,
                                                                  int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        auto statement = read_statement(500);
        if (statement) {
            // Check if this is the expected type
            std::string type;
            std::vector<std::string> fields;
            if (parse_statement(*statement, type, fields)) {
                if (type == expected_type) {
                    return statement;
                }
                // Log unexpected statement
                LogDebug << "Received unexpected statement type: " << type;
            }
        }
    }

    return std::nullopt;
}

std::string SatelliteTerminal::trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ============== Statement Parsing ==============

IcInfo SatelliteTerminal::parse_gsici(const std::vector<std::string>& fields,
                                        const std::string& raw) {
    IcInfo info;
    info.raw_statement = raw;
    info.valid = false;

    // Expected format: $GSICI,<type>,<status>,<signal>,<strength>,<lat>,<lon>,<alt>,<battery>,<power>,<work>*xx
    // Fields array contains fields after type (GSICI is parsed as type)
    // So fields[0] = type, fields[1] = status, etc.

    if (fields.size() < 10) {
        LogWarn << "GSICI statement has insufficient fields: " << fields.size();
        return info;
    }

    try {
        info.ic_type = std::stoi(fields[0]);
        info.ic_status = std::stoi(fields[1]);
        info.signal_quality = std::stoi(fields[2]);
        info.signal_strength = std::stoi(fields[3]);
        info.latitude = fields[4];
        info.longitude = fields[5];
        info.altitude = fields[6];
        info.battery_level = std::stoi(fields[7]);
        info.power_mode = std::stoi(fields[8]);
        info.work_mode = std::stoi(fields[9]);
        info.valid = true;
    } catch (const std::exception& e) {
        LogError << "Error parsing GSICI fields: " << e.what();
        return info;
    }

    return info;
}

DeviceInfo SatelliteTerminal::parse_gsdii(const std::vector<std::string>& fields,
                                           const std::string& raw) {
    DeviceInfo info;
    info.raw_statement = raw;
    info.valid = false;

    // Expected format: $GSDII,<type>,<hw1>,<hw2>,<sw_ver>,<module_ver>,<date>,<status>,<error>,<lat>,<lat_dir>,<lon>,<lon_dir>*xx
    // Fields array contains fields after type (GSDII is parsed as type)
    // So fields[0] = type, fields[1] = hw1, etc.

    if (fields.size() < 12) {
        LogWarn << "GSDII statement has insufficient fields: " << fields.size();
        return info;
    }

    try {
        info.device_type = std::stoi(fields[0]);
        info.hardware_ver1 = fields[1];
        info.hardware_ver2 = fields[2];
        info.software_version = fields[3];
        info.module_version = fields[4];
        info.manufacture_date = fields[5];
        info.status_flag = std::stoi(fields[6]);
        info.error_code = fields[7];
        info.latitude = fields[8];
        info.lat_direction = fields[9];
        info.longitude = fields[10];
        info.lon_direction = fields[11];
        info.valid = true;
    } catch (const std::exception& e) {
        LogError << "Error parsing GSDII fields: " << e.what();
        return info;
    }

    return info;
}

SignalInfo SatelliteTerminal::parse_gspdi(const std::vector<std::string>& fields,
                                           const std::string& raw) {
    SignalInfo info;
    info.raw_statement = raw;
    info.valid = false;

    // Expected format: $GSPDI,<time>,<mode>,<signal>,<f3>,<f4>,<f5>,<f6>,<quality>*xx
    // Example: $GSPDI,001838,1,158,0,0,1,1,45*72\r\n
    // Fields array contains fields after type (GSPDI is parsed as type)
    // So fields[0] = time, fields[1] = mode, etc.

    if (fields.size() < 8) {
        LogWarn << "GSPDI statement has insufficient fields: " << fields.size();
        return info;
    }

    try {
        info.timestamp = fields[0];
        info.mode = std::stoi(fields[1]);
        info.signal_strength = std::stoi(fields[2]);
        info.field3 = std::stoi(fields[3]);
        info.field4 = std::stoi(fields[4]);
        info.field5 = std::stoi(fields[5]);
        info.field6 = std::stoi(fields[6]);
        info.quality = std::stoi(fields[7]);
        info.valid = true;
    } catch (const std::exception& e) {
        LogError << "Error parsing GSPDI fields: " << e.what();
        return info;
    }

    return info;
}

} // namespace rpi