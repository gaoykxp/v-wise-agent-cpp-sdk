//
// Created by htsong on 2023/12/07.
//
#ifndef BASE_TOOLS_UDPSOCKET_H
#define BASE_TOOLS_UDPSOCKET_H

#include "dllhelper.h"

#include "basesocket.h"
#include <string>
#include <functional>
#include <thread>

namespace base_tools {
    class EASYSOCKET_API UDPSocket : public BaseSocket {
    public:
        std::function<void(const std::string&, const std::string&, std::uint16_t)> onMessageReceived;
        std::function<void(const char *, int, const std::string&, std::uint16_t)> onRawMessageReceived;

        explicit UDPSocket(bool useConnect = false, FDR_ON_ERROR, int socketId = -1);

        void SendTo(const std::string& message, const std::string& host, uint16_t port, FDR_ON_ERROR);

        void SendTo(const char *bytes, size_t byteslength, const std::string& host, uint16_t port, FDR_ON_ERROR);

        int Send(std::string message);

        int Send(const char *bytes, size_t byteslength);

        void Connect(const std::string& host, uint16_t port, FDR_ON_ERROR);

        void Connect(uint32_t ipv4, uint16_t port, FDR_ON_ERROR);

    private:
        static void Receive(UDPSocket *udpSocket);

        static void ReceiveFrom(UDPSocket *udpSocket);
    };
}
#endif
