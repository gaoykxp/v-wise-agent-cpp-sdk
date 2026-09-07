/**********************************************************************************************************************
    > File Name: rpi_at_channel_adapter.h
    > Description: AT 通道适配器（tbox::AtClient → vwise::modem::IAtChannel）
    >
    > 把 boards/rpi 的串口 AT 客户端适配为方言层驱动的传输接口，使 src/modem
    > 不依赖任何 boards 头文件（多模组架构的设计约束：方言层只认 HAL）。
    > 头文件即全部实现；RPIModuleInterface::atClient() 暴露底层通道。
**********************************************************************************************************************/
#pragma once

#include "modem_hal.h"
#include "rpi_at_client.h"

#include <chrono>

namespace vwise::modem {

class RpiAtChannelAdapter final : public IAtChannel {
public:
    explicit RpiAtChannelAdapter(tbox::AtClient& client) : client_(client) {}

    AtResult command(const std::string& cmd, int timeout_ms) override {
        auto rsp = client_.command(cmd, std::chrono::milliseconds(timeout_ms));
        AtResult r;
        r.ok = rsp.ok;
        r.lines = std::move(rsp.lines);
        r.error_text = std::move(rsp.err);
        return r;
    }

    void addUrcHandler(const std::string& prefix, AtUrcCallback cb) override {
        client_.register_urc_handler(prefix, std::move(cb));
    }

private:
    tbox::AtClient& client_;
};

} // namespace vwise::modem
