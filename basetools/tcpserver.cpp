//
// Created by htsong on 2023/12/07.
//
#include "tcpserver.h"
#include <iostream>

namespace base_tools {
    TCPServer::TCPServer(std::function<void(int, const std::string&)> onError) : BaseSocket(onError, TCP) {
        int opt = 1;
        setsockopt(this->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int));
        setsockopt(this->sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(int));
    }

    void TCPServer::Bind(int port, std::function<void(int, const std::string&)> onError) {
        this->Bind("0.0.0.0", port, onError);
    }

    void TCPServer::Bind(const char *address, uint16_t port, std::function<void(int, const std::string&)> onError) {
        if (inet_pton(AF_INET, address, &this->address.sin_addr) <= 0) {
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

    void TCPServer::Listen(std::function<void(int, const std::string&)> onError) {
        if (listen(this->sock, 10) < 0) {
            onError(errno, "Error: Server can't listen the socket.");
            return;
        }

        std::thread acceptThread(&TCPServer::Accept_, this, onError);
        acceptThread.detach();
    }

    void TCPServer::Close() {
        shutdown(this->sock, SHUT_RDWR);

        BaseSocket::Close();
    }

    void TCPServer::Send(const std::string &message) const {
        std::string msg(message);
        msg.append("\n");
        std::cout << __FILE__ << ":" << __FUNCTION__ << ":" << __LINE__ << " msg: " << msg << std::endl;
        for (auto pConnect: connectlist_) {
            int ret = pConnect->Send(msg);
            std::cout << "Send ret: " << ret << std::endl;
        }
    }

    void TCPServer::Send(const char *bytes, size_t byteslength) const {
        std::cout << __FILE__ << ":" << __FUNCTION__ << ":" << __LINE__ << " msg: ";
        for (int i = 0; i < byteslength; i++)
            printf(" %02x", bytes[i]);
        std::cout << std::endl;

        for (auto pConnect: connectlist_) {
            int ret = pConnect->Send(bytes, byteslength);
            std::cout << "Send ret: " << ret << std::endl;
        }
    }

    void TCPServer::Accept_(std::function<void(int, const std::string&)> onError) {
        sockaddr_in newSocketInfo;
        socklen_t newSocketInfoLength = sizeof(newSocketInfo);

        int newSock;
        while (!isClosed) {
            while ((newSock = accept(sock, (sockaddr *) &newSocketInfo, &newSocketInfoLength)) < 0) {
                if (errno == EBADF || errno == EINVAL) return;

                onError(errno, "Error while accepting a new connection.");
                return;
            }

            if (!isClosed && newSock >= 0) {
                TCPSocket *newSocket = new TCPSocket([](int e, std::string er) {
                    FDR_UNUSED(e);
                    FDR_UNUSED(er);
                }, newSock);
                newSocket->setAddressStruct(newSocketInfo);
                connectlist_.push_back(newSocket);
                onNewConnection(newSocket);
                newSocket->Listen();
            }
        }
    }

    void TCPServer::removeSock(TCPSocket *socket) {
        connectlist_.remove(socket);
    }
}