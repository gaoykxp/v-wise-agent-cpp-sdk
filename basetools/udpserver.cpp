//
// Created by htsong on 2023/12/07.
//
#include "udpserver.h"

namespace base_tools {
    UDPServer::UDPServer() {
    }

    void UDPServer::Bind(int port, std::function<void(int, const std::string&)> onError) {
        this->Bind("0.0.0.0", port, onError);
    }

    void UDPServer::Bind(const std::string& IPv4, std::uint16_t port, std::function<void(int, const std::string&)> onError) {
        if (inet_pton(AF_INET, IPv4.c_str(), &this->address.sin_addr) <= 0) {
            onError(errno, "Invalid address. Address type not supported.");
            return;
        }

        this->address.sin_family = AF_INET;
        this->address.sin_port = htons(port);

        if (bind(this->sock, (const sockaddr *) &this->address, sizeof(this->address)) < 0) {
            onError(errno, "Cannot bind the socket.");
            return;
        }
    }
}