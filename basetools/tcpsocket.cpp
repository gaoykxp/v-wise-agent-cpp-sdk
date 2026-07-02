//
// Created by htsong on 2023/12/07.
//
#include "tcpsocket.h"
#include <string.h>
#include <iostream>

namespace base_tools {
    TCPSocket::TCPSocket(std::function<void(int, const std::string&)> onError, int socketId) : BaseSocket(onError, TCP,
                                                                                                   socketId) {
    }

    int TCPSocket::Send(const std::string &message) {
        std::cout << __FILE__ << ":" << __FUNCTION__ << ":" << __LINE__ << ", msg: " << message << std::endl;
        return this->Send(message.c_str(), message.length());
    }

    int TCPSocket::Send(const char *bytes, size_t byteslength) {
//    std::cout << __FILE__ <<  ":" << __FUNCTION__ <<   ":" << __LINE__ << std::endl;
        if (this->isClosed)
            return -1;
        int sent = 0;
        if ((sent = ::send(this->sock, bytes, byteslength, 0)) < 0) {
            perror("send");
        }
//    std::cout << __FILE__ <<  ":" << __FUNCTION__ <<   ":" << __LINE__ << std::endl;
        return sent;
    }

    void TCPSocket::Connect(const std::string& host, uint16_t port, std::function<void()> onConnected,
                            std::function<void(int, const std::string&)> onError) {
        struct addrinfo hints, *res, *it;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        int status;
        if ((status = getaddrinfo(host.c_str(), NULL, &hints, &res)) != 0) {
            onError(errno, "Invalid address." + std::string(gai_strerror(status)));
            return;
        }

        for (it = res; it != NULL; it = it->ai_next) {
            if (it->ai_family == AF_INET) { // IPv4
                memcpy((void *) (&this->address), (void *) it->ai_addr, sizeof(sockaddr_in));
                break; // for now, just get first ip (ipv4).
            }
        }

        freeaddrinfo(res);

        this->Connect((uint32_t) this->address.sin_addr.s_addr, port, onConnected, onError);
    }

    void TCPSocket::Connect(uint32_t ipv4, uint16_t port, std::function<void()> onConnected,
                            std::function<void(int, const std::string&)> onError) {
        this->address.sin_family = AF_INET;
        this->address.sin_port = htons(port);
        this->address.sin_addr.s_addr = ipv4;

        // Try to connect.
        if (connect(this->sock, (const sockaddr *) &this->address, sizeof(sockaddr_in)) < 0) {
            onError(errno, "Connection failed to the host.");
            return;
        }

        // Connected to the server, fire the event.
        onConnected();

        // Start listening from server:
        this->Listen();
    }

    void TCPSocket::Listen() {
        // Start listening the socket from thread.
        std::thread receiveListening(Receive, this);
        receiveListening.detach();
    }

    void TCPSocket::setAddressStruct(sockaddr_in addr) {
        this->address = addr;
    }

    void TCPSocket::Receive(TCPSocket *socket) {
        char tempBuffer[socket->BUFFER_SIZE];
        int messageLength;

        while ((messageLength = recv(socket->sock, tempBuffer, socket->BUFFER_SIZE, 0)) > 0) {
            tempBuffer[messageLength] = '\0';
            if (socket->onMessageReceived)
                socket->onMessageReceived(std::string(tempBuffer).substr(0, messageLength));

            if (socket->onRawMessageReceived)
                socket->onRawMessageReceived(tempBuffer, messageLength);
        }

        socket->Close();
        if (socket->onSocketClosed)
            socket->onSocketClosed();
    }
}