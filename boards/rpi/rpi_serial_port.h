#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace tbox {

// Minimal serial port wrapper.
// Currently implemented for Linux in src/tbox/serial_port_linux.cpp
class SerialPort
{
public:
    SerialPort() = default;
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    SerialPort(SerialPort&&) = delete;
    SerialPort& operator=(SerialPort&&) = delete;

    // Open like "COM3" or "\\\\.\\COM3"
    bool open(const std::string& port_name, int baud_rate = 115200);
    void close();
    bool is_open() const;

    // Write bytes as-is (no newline added)
    bool write(const std::string& data);

    // Read some bytes (non-blocking-ish with timeout)
    // Returns empty string on timeout/no data.
    std::string read_some(std::chrono::milliseconds timeout);

    // Read until delimiter or timeout. Delimiter is included in returned string.
    std::string read_until(char delim, std::chrono::milliseconds timeout);

private:
    int fd_{-1};
};

} // namespace tbox

