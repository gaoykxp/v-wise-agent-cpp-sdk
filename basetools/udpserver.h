//
// Created by htsong on 2023/12/07.
//
#ifndef BASE_TOOLS_UDPSERVER_H
#define BASE_TOOLS_UDPSERVER_H

#include "dllhelper.h"

#include "udpsocket.h"
#include <string>

namespace base_tools {
    class EASYSOCKET_API UDPServer : public UDPSocket {
    public:
        UDPServer();

        void Bind(int port, FDR_ON_ERROR);

        void Bind(const std::string& IPv4, std::uint16_t port, FDR_ON_ERROR);
    };
}
#endif
