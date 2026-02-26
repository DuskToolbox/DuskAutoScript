#ifndef DAS_PLUGINS_IPCTESTPLUGIN1_PLUGINIMPL_H
#define DAS_PLUGINS_IPCTESTPLUGIN1_PLUGINIMPL_H

#include <cstdint>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasInput.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTouch.Implements.hpp>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    IpcTestPlugin1,
    0x2a3b4c5d,
    0x6e7f,
    0x4b5c,
    0x9d,
    0x0e,
    0x1f,
    0x2a,
    0x3b,
    0x4c,
    0x5d,
    0x6e);

DAS_NS_BEGIN

/**
 * @brief IDasTouch Mock 实现类
 *
 * 用于 IPC 端到端测试，实现 IDasTouch 接口。
 * Click 和 Swipe 方法返回 DAS_S_OK（Mock 实现）。
 */
class DasTouchMockImpl final
    : public PluginInterface::DasTouchImplBase<DasTouchMockImpl>
{
public:
    explicit DasTouchMockImpl(uint16_t session_id);

    // IDasTypeInfo methods (必须手动实现，因为 DasTouchImplBase 没有实现它们)
    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    // IDasTouch methods
    DAS_IMPL Click(int32_t x, int32_t y) override;
    DAS_IMPL Swipe(
        PluginInterface::DasPoint from,
        PluginInterface::DasPoint to,
        int32_t                   duration_ms) override;

private:
    uint16_t session_id_ = 0;
};

/**
 * @brief IPC 测试插件1 - 实现 IDasPluginPackage
 *
 * 该插件提供 IDasTouch 接口的工厂，用于 IPC 端到端测试。
 */
class IpcTestPlugin1 final
    : public PluginInterface::DasPluginPackageImplBase<IpcTestPlugin1>
{
public:
    // 设置 session_id（由 Host 进程调用）
    void SetSessionId(uint16_t session_id);

    // IDasPluginPackage methods
    DAS_IMPL EnumFeature(
        const size_t                       index,
        PluginInterface::DasPluginFeature* p_out_feature) override;

    DAS_IMPL CreateFeatureInterface(size_t index, IDasBase** pp_out_interface)
        override;

    DAS_IMPL CanUnloadNow(bool* p_can_unload) override;

private:
    uint16_t session_id_ = 0;
};

DAS_NS_END

#endif // DAS_PLUGINS_IPCTESTPLUGIN1_PLUGINIMPL_H
