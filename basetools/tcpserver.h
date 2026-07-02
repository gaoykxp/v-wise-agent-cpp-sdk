//
// Created by htsong on 2023/12/07.
//
#ifndef BASE_TOOLS_TCPSERVER_H
#define BASE_TOOLS_TCPSERVER_H

#include "dllhelper.h"

#include "tcpsocket.h"
#include <string>
#include <functional>
#include <thread>
#include <list>

namespace base_tools {
    class EASYSOCKET_API TCPServer : public BaseSocket {
    public:
        // Event Listeners:
        std::function<void(TCPSocket *)> onNewConnection = [](TCPSocket *sock) {FDR_UNUSED(sock)};

        explicit TCPServer(FDR_ON_ERROR);

        // Binding the server.
        void Bind(int port, FDR_ON_ERROR);

        void Bind(const char *address, uint16_t port, FDR_ON_ERROR);

        // Start listening the server.
        void Listen(FDR_ON_ERROR);

        // Overriding Close to add shutdown():
        void Close() override;

        void Send(const std::string &message) const;

        void Send(const char *bytes, size_t byteslength) const;

        void removeSock(TCPSocket *socket);

    private:
        void Accept_(FDR_ON_ERROR);

    private:
        std::list<TCPSocket *> connectlist_;
    };
}
#endif
