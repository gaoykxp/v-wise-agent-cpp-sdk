#include "rpi_serial_port.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace tbox {

static speed_t baud_to_flag(int baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default: return B115200;
    }
}

SerialPort::~SerialPort()
{
    close();
}

bool SerialPort::open(const std::string& port_name, int baud_rate)
{
    close();

    const int fd = ::open(port_name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return false;

    termios tty{};
    if (::tcgetattr(fd, &tty) != 0) {
        ::close(fd);
        return false;
    }

    ::cfmakeraw(&tty);

    const speed_t spd = baud_to_flag(baud_rate);
    ::cfsetispeed(&tty, spd);
    ::cfsetospeed(&tty, spd);

    // 8N1
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);

    // Non-canonical, no internal read timeout here (we use select)
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (::tcsetattr(fd, TCSANOW, &tty) != 0) {
        ::close(fd);
        return false;
    }

    // Switch back to blocking reads; we'll still gate with select() per call.
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    fd_ = fd;
    return true;
}

void SerialPort::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool SerialPort::is_open() const
{
    return fd_ >= 0;
}

bool SerialPort::write(const std::string& data)
{
    if (fd_ < 0)
        return false;

    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
        const ssize_t n = ::write(fd_, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

std::string SerialPort::read_some(std::chrono::milliseconds timeout)
{
    if (fd_ < 0)
        return {};

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd_, &rfds);

    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);

    const int rc = ::select(fd_ + 1, &rfds, nullptr, nullptr, &tv);
    if (rc <= 0)
        return {}; // timeout or error

    char buf[512];
    const ssize_t n = ::read(fd_, buf, sizeof(buf));
    if (n <= 0)
        return {};
    return std::string(buf, buf + n);
}

std::string SerialPort::read_until(char delim, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string out;

    while (std::chrono::steady_clock::now() < deadline) {
        auto chunk = read_some(std::chrono::milliseconds(200));
        if (!chunk.empty()) {
            out += chunk;
            if (out.find(delim) != std::string::npos)
                break;
        }
    }
    return out;
}

} // namespace tbox

