#ifndef DAS_PLUGINS_IPCTESTPLUGIN1_PLUGINIMPL_H
#define DAS_PLUGINS_IPCTESTPLUGIN1_PLUGINIMPL_H

#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/abi/IDasInput.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasComponentFactory.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTouch.Implements.hpp>

// {8A5B6C7D-9E0F-1A2B-3C4D-5E6F7A8B9C0D}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    DasQueryComponentImpl,
    0x8a5b6c7d,
    0x9e0f,
    0x1a2b,
    0x3c,
    0x4d,
    0x5e,
    0x6f,
    0x7a,
    0x8b,
    0x9c,
    0x0d);

// {1B2C3D4E-5F6A-7B8C-9D0E-1F2A3B4C5D6E}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    DasQueryComponentFactoryImpl,
    0x1b2c3d4e,
    0x5f6a,
    0x7b8c,
    0x9d,
    0x0e,
    0x1f,
    0x2a,
    0x3b,
    0x4c,
    0x5d,
    0x6e);

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
    DasTouchMockImpl() = default;

    // IDasTypeInfo methods (必须手动实现，因为 DasTouchImplBase 没有实现它们)
    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    // IDasTouch methods
    DAS_IMPL Click(int32_t x, int32_t y) override;
    DAS_IMPL Swipe(
        PluginInterface::DasPoint from,
        PluginInterface::DasPoint to,
        int32_t                   duration_ms) override;
};

/**
 * @brief IDasComponent 实现 — 查询主进程接口
 *
 * 提供 Dispatch("queryMainProcessString") 和
 * Dispatch("queryMainProcessVariantVector") 方法，
 * 通过 DasQueryMainProcessInterface 获取主进程中注册的接口。
 */
class DasQueryComponentImpl final
    : public PluginInterface::DasComponentImplBase<DasQueryComponentImpl>
{
public:
    DasQueryComponentImpl() = default;

    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL Dispatch(
        IDasReadOnlyString*                  p_function_name,
        ExportInterface::IDasVariantVector*  p_arguments,
        ExportInterface::IDasVariantVector** pp_out_result) override;

private:
    DasResult HandleQueryMainProcessString(
        ExportInterface::IDasVariantVector** pp_out_result);

    DasResult HandleQueryMainProcessVariantVector(
        ExportInterface::IDasVariantVector** pp_out_result);
};

/**
 * @brief IDasComponentFactory 实现
 *
 * 创建 DasQueryComponentImpl 实例。
 */
class DasQueryComponentFactoryImpl final
    : public PluginInterface::DasComponentFactoryImplBase<
          DasQueryComponentFactoryImpl>
{
public:
    DasQueryComponentFactoryImpl() = default;

    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL IsSupported(const DasGuid& component_iid) override;
    DAS_IMPL CreateInstance(
        const DasGuid&                   component_iid,
        PluginInterface::IDasComponent** pp_out_component) override;
};

/**
 * @brief IPC 测试插件1 - 实现 IDasPluginPackage
 *
 * 该插件提供：
 * - index=0: IDasTouch 工厂（DasTouchMockImpl）
 * - index=1: IDasComponentFactory 工厂（DasQueryComponentFactoryImpl）
 */
class IpcTestPlugin1 final
    : public PluginInterface::DasPluginPackageImplBase<IpcTestPlugin1>
{
public:
    // IDasPluginPackage methods
    DAS_IMPL EnumFeature(
        const size_t                       index,
        PluginInterface::DasPluginFeature* p_out_feature) override;

    DAS_IMPL CreateFeatureInterface(size_t index, IDasBase** pp_out_interface)
        override;

    DAS_IMPL CanUnloadNow(bool* p_can_unload) override;
};

DAS_NS_END

#endif // DAS_PLUGINS_IPCTESTPLUGIN1_PLUGINIMPL_H
