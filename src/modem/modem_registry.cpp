/**********************************************************************************************************************
    > File Name: modem_registry.cpp
    > Description: 模组驱动编译期绑定（业务层与方言层的唯一连接点）
    >
    > CMake 选项 VWISE_MODEM=quectel_5g → 编译定义 VWISE_MODEM_DRIVER_QUECTEL_5G
    > → 本文件选中 Quectel5GDriver。业务层一律通过 modemDriver() 取驱动，
    > 全 SDK 只允许这一个文件出现驱动选择逻辑。
    >
    > 新增模组三步：drivers/<vendor>/<model>/ 实现驱动 → 此处加一个 #elif →
    > CMakeLists 校验列表加一项。未识别的 VWISE_MODEM 在 CMake 配置期即报错。
**********************************************************************************************************************/
#include "modem_hal.h"

#if defined(VWISE_MODEM_DRIVER_QUECTEL_5G)
#include "drivers/quectel_5g/quectel_5g_driver.h"
namespace vwise::modem {
IModemDriver& modemDriver() {
    static Quectel5GDriver inst;
    return inst;
}
} // namespace vwise::modem

// ---- 未来模组在此追加，例如： ----
// #elif defined(VWISE_MODEM_DRIVER_SIMCOM_A76XX)
// #include "drivers/simcom_a76xx/simcom_a76xx_driver.h"
// ...

#elif defined(VWISE_MODEM_STUB)
// 单元测试/无模组环境：空驱动（全部能力缺失，语义层自然降级）
namespace vwise::modem {
namespace {
class StubDriver final : public IModemDriver {
public:
    const ModemProfile& profile() const override {
        static const ModemProfile p{};
        return p;
    }
    bool init(IAtChannel&) override { return true; }
    bool getDiagSnapshot(DiagSnapshot&) override { return false; }
    bool getServingSignal(SignalInfo&, ServingCell&) override { return false; }
    bool getExtendedError(std::string&) override { return false; }
};
} // namespace
IModemDriver& modemDriver() {
    static StubDriver inst;
    return inst;
}
} // namespace vwise::modem

#else
#error "未选择模组驱动：请配置 CMake 选项 -DVWISE_MODEM=<quectel_5g|...>（见 CMakeLists.txt 支持列表）"
#endif
