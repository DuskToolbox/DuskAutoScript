#ifndef DAS_CORE_UTILS_IDASSTOPTOKENIMPL_H
#define DAS_CORE_UTILS_IDASSTOPTOKENIMPL_H

#include <DAS/_autogen/idl/abi/IDasTask.h>
#include <DAS/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <atomic>
#include <das/Core/Utils/Config.h>

DAS_CORE_UTILS_NS_BEGIN

/**
 * @brief 构造复制拷贝移动必须保证线程安全
 */
class DasStopTokenImplOnStack final
    : public Das::PluginInterface::DasStopTokenImplBase<DasStopTokenImplOnStack>
{
    // DasStopTokenImplBase provides on-stack semantics (AddRef/Release return
    // 1)
    uint32_t AddRef() override;
    uint32_t Release() override;

public:
    DasResult StopRequested() override;

    void RequestStop();
    void Reset();

    template <typename... Args>
    static DasPtr<IDasStopToken> Make(Args&&... args) = delete;

    template <typename... Args>
    static DasStopTokenImplOnStack* MakeRaw(Args&&... args) = delete;

private:
    std::atomic_bool is_stop_requested_{false};
};

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_IDASSTOPTOKENIMPL_H
