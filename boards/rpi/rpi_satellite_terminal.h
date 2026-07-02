/**********************************************************************************************************************
    > File Name: rpi_satellite_terminal.h
    > Author: Satellite Terminal Protocol Implementation
    > Date: 05/09/26
    > Description: Satellite terminal communication based on ASCII protocol

    Protocol format (NMEA-like):
    $<type>,<field1>,<field2>,...*<checksum><CR><LF>

    UART settings:
    - Baud rate: 115200bps
    - Data bits: 8bit
    - Stop bits: 1bit
    - Parity: None

    Commands:
    - IC Query:   Send $CCGMO,ICI,2,0*28\r\n  -> Receive $GSICI,...
    - Version:    Send $CCGMO,DII,2,0*2F\r\n  -> Receive $GSDII,...
    - Signal:     Send $CCGMO,PDI,2,1*37\r\n  -> Receive $GSPDI,...
**********************************************************************************************************************/
#pragma once

#include "rpi_serial_port.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <queue>
#include <condition_variable>

namespace rpi {

// IC (Information) response data - parsed from $GSICI statement
// Format: $GSICI,<type>,<status>,<signal>,<strength>,<lat>,<lon>,<alt>,<battery>,<power>,<work>*xx
struct IcInfo {
    int ic_type{0};             // Field 1: IC type (e.g., 10)
    int ic_status{0};           // Field 2: IC status (e.g., 0)
    int signal_quality{0};      // Field 3: Signal quality (e.g., 0)
    int signal_strength{0};     // Field 4: Signal strength (e.g., 30)
    std::string latitude;       // Field 5: Latitude (e.g., "x" for unknown)
    std::string longitude;      // Field 6: Longitude
    std::string altitude;       // Field 7: Altitude
    int battery_level{0};       // Field 8: Battery level (e.g., 50)
    int power_mode{0};          // Field 9: Power mode (e.g., 1)
    int work_mode{0};           // Field 10: Work mode (e.g., 1)
    bool valid{false};
    std::string raw_statement;  // Raw statement for debugging
};

// Device information - parsed from $GSDII statement
// Format: $GSDII,<type>,<hw1>,<hw2>,<sw_ver>,<module_ver>,<date>,<status>,<error>,<lat>,<lat_dir>,<lon>,<lon_dir>*xx
struct DeviceInfo {
    int device_type{0};             // Field 1: Device type (e.g., 1)
    std::string hardware_ver1;      // Field 2: Hardware version 1
    std::string hardware_ver2;      // Field 3: Hardware version 2
    std::string software_version;   // Field 4: Software version (e.g., "SOC_V1.2_100_250225_T3")
    std::string module_version;     // Field 5: Module version (e.g., "GM1001_V1.1")
    std::string manufacture_date;   // Field 6: Manufacture date (e.g., "20221211")
    int status_flag{0};             // Field 7: Status flag (e.g., 0)
    std::string error_code;         // Field 8: Error code (e.g., "000004")
    std::string latitude;           // Field 9: Latitude value (e.g., "0000.000000")
    std::string lat_direction;      // Field 10: Latitude direction (N/S)
    std::string longitude;          // Field 11: Longitude value
    std::string lon_direction;      // Field 12: Longitude direction (E/W)
    bool valid{false};
    std::string raw_statement;      // Raw statement for debugging
};

// Signal information - parsed from $GSPDI statement
// Format: $GSPDI,<time>,<field1>,<signal>,<field3>,<field4>,<field5>,<field6>,<field7>*xx
// Example: $GSPDI,001838,1,158,0,0,1,1,45*72\r\n
struct SignalInfo {
    std::string timestamp;          // Field 1: UTC time (e.g., "001838" = 00:18:38)
    int mode{0};                    // Field 2: Mode indicator (e.g., 1)
    int signal_strength{0};         // Field 3: Signal strength (e.g., 158)
    int field3{0};                  // Field 4: Reserved field
    int field4{0};                  // Field 5: Reserved field
    int field5{0};                  // Field 6: Status indicator
    int field6{0};                  // Field 7: Status indicator
    int quality{0};                 // Field 8: Signal quality (e.g., 45)
    bool valid{false};
    std::string raw_statement;      // Raw statement for debugging
};

// Terminal configuration
struct SatelliteTerminalConfig {
    std::string port_name;              // Serial port (e.g., "/dev/ttyUSB1")
    int baud_rate{115200};              // Baud rate (default: 115200)
    int read_timeout_ms{2000};          // Read timeout in milliseconds
    int query_timeout_ms{3000};         // Query timeout for responses
};

// Callback types
using IcInfoCallback = std::function<void(const IcInfo& info)>;
using DeviceInfoCallback = std::function<void(const DeviceInfo& info)>;
using SignalInfoCallback = std::function<void(const SignalInfo& info)>;

/**
 * @brief Satellite terminal communication class
 *
 * Implements ASCII protocol communication with satellite terminal via UART.
 * Protocol format: $<type>,<field1>,<field2>,...*<checksum><CR><LF>
 */
class SatelliteTerminal {
public:
    SatelliteTerminal();
    ~SatelliteTerminal();

    // Non-copyable
    SatelliteTerminal(const SatelliteTerminal&) = delete;
    SatelliteTerminal& operator=(const SatelliteTerminal&) = delete;

    // ============== Connection Management ==============

    /**
     * @brief Open terminal connection
     * @param config Terminal configuration
     * @return true if successful
     */
    bool open(const SatelliteTerminalConfig& config);

    /**
     * @brief Close terminal connection
     */
    void close();

    /**
     * @brief Check if terminal is connected
     */
    bool is_open() const;

    // ============== Query Operations ==============

    /**
     * @brief Query IC information from satellite terminal
     * Send: $CCGMO,ICI,2,0*28\r\n
     * Receive: $GSICI,<type>,<status>,<signal>,<strength>,<lat>,<lon>,<alt>,<battery>,<power>,<work>*xx\r\n
     * @param timeout_ms Query timeout (default 3000ms)
     * @return IC info if successful
     */
    std::optional<IcInfo> query_ic_info(int timeout_ms = 3000);

    /**
     * @brief Query device information/version from satellite terminal
     * Send: $CCGMO,DII,2,0*2F\r\n
     * Receive: $GSDII,<type>,<hw1>,<hw2>,<sw_ver>,<module_ver>,<date>,<status>,<error>,<lat>,<lat_dir>,<lon>,<lon_dir>*xx\r\n
     * @param timeout_ms Query timeout (default 3000ms)
     * @return Device info if successful
     */
    std::optional<DeviceInfo> query_device_info(int timeout_ms = 3000);

    /**
     * @brief Query signal information from satellite terminal
     * Send: $CCGMO,PDI,2,1*37\r\n
     * Receive: $GSPDI,<time>,<mode>,<signal>,<f3>,<f4>,<f5>,<f6>,<quality>*xx\r\n
     * Example: $GSPDI,001838,1,158,0,0,1,1,45*72\r\n
     * @param timeout_ms Query timeout (default 3000ms)
     * @return Signal info if successful
     */
    std::optional<SignalInfo> query_signal_info(int timeout_ms = 3000);

    // ============== Raw Statement Operations ==============

    /**
     * @brief Send raw statement to terminal
     * @param statement Complete statement (will add checksum if not present)
     * @return true if sent successfully
     */
    bool send_statement(const std::string& statement);

    /**
     * @brief Read statement from terminal (synchronous)
     * @param timeout_ms Read timeout
     * @return Statement string if received
     */
    std::optional<std::string> read_statement(int timeout_ms);

    // ============== Callbacks ==============

    /**
     * @brief Set callback for IC info received (for async mode)
     */
    void set_ic_info_callback(IcInfoCallback callback);

    /**
     * @brief Set callback for device info received (for async mode)
     */
    void set_device_info_callback(DeviceInfoCallback callback);

    /**
     * @brief Set callback for signal info received (for async mode)
     */
    void set_signal_info_callback(SignalInfoCallback callback);

    // ============== Protocol Utilities ==============

    /**
     * @brief Calculate XOR checksum for statement content
     * XOR of all characters between '$' and '*' (exclusive)
     * @param content String content to calculate checksum
     * @return Two-character hex checksum string (uppercase)
     */
    static std::string calculate_checksum(const std::string& content);

    /**
     * @brief Verify checksum of received statement
     * @param statement Full statement including $ and *checksum
     * @return true if checksum is valid
     */
    static bool verify_checksum(const std::string& statement);

    /**
     * @brief Build a complete statement with checksum
     * @param type Statement type identifier (e.g., "CCGMO")
     * @param fields Data fields
     * @return Complete statement with checksum and terminators (\r\n)
     */
    static std::string build_statement(const std::string& type, const std::vector<std::string>& fields);

    /**
     * @brief Parse received statement into type and fields
     * @param statement Raw statement string
     * @param type Output: statement type
     * @param fields Output: data fields
     * @return true if parsing successful and checksum valid
     */
    static bool parse_statement(const std::string& statement, std::string& type, std::vector<std::string>& fields);

private:
    // Parse specific statement types
    static IcInfo parse_gsici(const std::vector<std::string>& fields, const std::string& raw);
    static DeviceInfo parse_gsdii(const std::vector<std::string>& fields, const std::string& raw);
    static SignalInfo parse_gspdi(const std::vector<std::string>& fields, const std::string& raw);

    // Helper: wait for specific response type
    std::optional<std::string> wait_for_response(const std::string& expected_type, int timeout_ms);

    // Helper: trim whitespace
    static std::string trim(const std::string& s);

    // Members
    tbox::SerialPort port_;
    SatelliteTerminalConfig config_;
    std::atomic<bool> connected_{false};

    // Receive buffer
    std::mutex buffer_mutex_;
    std::string receive_buffer_;

    // Callbacks
    std::mutex callback_mutex_;
    IcInfoCallback ic_info_callback_;
    DeviceInfoCallback device_info_callback_;
    SignalInfoCallback signal_info_callback_;
};

} // namespace rpi