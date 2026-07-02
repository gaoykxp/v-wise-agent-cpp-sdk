//
// Created by htsong on 2023/12/07.
//
#ifndef BASE_TOOLS_TCPSOCKET_H
#define BASE_TOOLS_TCPSOCKET_H

#include "dllhelper.h"

#include "basesocket.h"
#include <string>
#include <functional>
#include <thread>

namespace base_tools {
    class EASYSOCKET_API TCPSocket : public BaseSocket {
    public:
        // Event Listeners:
        std::function<void(const std::string&)> onMessageReceived;
        std::function<void(const char *, int)> onRawMessageReceived;
        std::function<void()> onSocketClosed;

        explicit TCPSocket(FDR_ON_ERROR, int socketId = -1);

        int Send(const std::string &message);

        int Send(const char *bytes, size_t byteslength);

        void Connect(const std::string& host, uint16_t port, std::function<void()> onConnected = []() {}, FDR_ON_ERROR);

        void Connect(uint32_t ipv4, uint16_t port, std::function<void()> onConnected = []() {}, FDR_ON_ERROR);

        void Listen();

        void setAddressStruct(sockaddr_in addr);

    private:
        static void Receive(TCPSocket *socket);
    };
}
#endif  // BASE_TOOLS_TCPSOCKET_H
