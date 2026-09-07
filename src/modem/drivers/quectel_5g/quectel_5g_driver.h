/**********************************************************************************************************************
    > File Name: quectel_5g_driver.h
    > Description: 移远 5G 模组方言层驱动（RG520N / RM520N / RG52xF 同族）
    >
    > 职责（多模组差异的唯一存在地）：
    >   1. AT 指令选择：C5GREG(标准)→Q5GREG(Quectel)→CEREG 兜底；QENG/CESQ/CGPADDR/QADC/CEER
    >   2. 响应解析：私有值 → 3GPP 标准值（RegInfo/SignalInfo/ServingCell...）
    *   3. URC 翻译：私有 URC 前缀 → DiagEvent 语义事件发布到 DiagEventBus
    >
    > 依据：Quectel RG520N AT Commands Manual V1.0.0
    >   §2.23 CMEE / §3.2 CEER / §5.5 C5GREG / §5.20 QENG / §9.7 CGEREP / §11.3 QADC
**********************************************************************************************************************/
#pragma once

#include <string>

#include "modem_hal.h"

namespace vwise::modem {

class Quectel5GDriver final : public IModemDriver {
public:
    const ModemProfile& profile() const override;

    // 绑定 AT 通道；使能 CMEE=2 / CEREG=3 / C5GREG=3 / CGEREP=1 / QNETDEVSTATUS=1
    // 并注册 URC 处理器（与 RPIModuleInterface::init 的启动序列幂等，重复下发无害；
    // CEREG/C5GREG n=3 不被老固件支持时降级 n=2）
    bool init(IAtChannel& ch) override;

    bool getDiagSnapshot(DiagSnapshot& out) override;
    bool getServingSignal(SignalInfo& sig, ServingCell& cell) override;
    bool getExtendedError(std::string& text) override;

private:
    // ---- 查询子项（方言层内部，签名一律返回标准结构）----
    void queryRegistration(RegInfo& out);
    void queryServing(SignalInfo& sig, ServingCell& cell);
    void queryLteSignalByCesq(SignalInfo& sig);      // AT+CESQ 兜底（无 QENG LTE 行时）
    void queryBearer(DataBearer& out);               // AT+CGPADDR=1 → AT+CGPADDR

    // ---- URC 处理器（翻译为 DiagEvent 发布）----
    void onCgev(const std::string& line);
    void onQsimstat(const std::string& line);
    void onCpin(const std::string& line);
    void onRegUrc(const std::string& line, const char* domainHint);  // +CEREG:/+C5GREG: stat=3
    void onReady(const std::string& line);
    void onPoweredDown(const std::string& line);
    void onCfun(const std::string& line);
    void onQind(const std::string& line);
    void onQnetdevstatus(const std::string& line);
    void registerUrcHandlers();

    static void emit(DiagEvent ev, const std::string& raw);

    IAtChannel* ch_{nullptr};
};

} // namespace vwise::modem
