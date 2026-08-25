#ifndef DAS_PLUGINS_IPCTESTPLUGIN2_PLUGINIMPL_H
#define DAS_PLUGINS_IPCTESTPLUGIN2_PLUGINIMPL_H

#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasComponentFactory.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentHost.Implements.hpp>

// {7F4AFAA1-C3AA-4933-9F1F-D4CD4D8BED6A}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    DasComponentImpl,
    0x7f4afaa1,
    0xc3aa,
    0x4933,
    0x9f,
    0x1f,
    0xd4,
    0xcd,
    0x4d,
    0x8b,
    0xed,
    0x6a);

// {B0CBB921-748F-4ED0-AF98-EB5B59E3E574}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    DasComponentFactoryImpl,
    0xb0cbb921,
    0x748f,
    0x4ed0,
    0xaf,
    0x98,
    0xeb,
    0x5b,
    0x59,
    0xe3,
    0xe5,
    0x74);

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    IpcTestPlugin2,
    0x2b3c4d5e,
    0x6f7a,
    0x5b6c,
    0x9d,
    0x0e,
    0x1f,
    0x2a,
    0x3b,
    0x4c,
    0x5d,
    0x6e);

// {0BF06F77-FDAD-433D-887D-9DC0A783F8B0}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    TestTaskComponentHostFeature,
    0x0bf06f77,
    0xfdad,
    0x433d,
    0x88,
    0x7d,
    0x9d,
    0xc0,
    0xa7,
    0x83,
    0xf8,
    0xb0);

// {4955996C-7B87-4D86-8561-9052A9999AA8}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    EchoTaskComponent,
    0x4955996c,
    0x7b87,
    0x4d86,
    0x85,
    0x61,
    0x90,
    0x52,
    0xa9,
    0x99,
    0x9a,
    0xa8);

DAS_NS_BEGIN

class DasComponentImpl final
    : public PluginInterface::DasComponentImplBase<DasComponentImpl>
{
public:
    DasComponentImpl() = default;

    // IDasTypeInfo methods (必须手动实现)
    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL Dispatch(
        IDasReadOnlyString*                  p_function_name,
        ExportInterface::IDasVariantVector*  p_arguments,
        ExportInterface::IDasVariantVector** pp_out_result) override;

private:
    DasResult HandleEcho(
        ExportInterface::IDasVariantVector*  args,
        ExportInterface::IDasVariantVector** out);
    DasResult HandleCompute(
        ExportInterface::IDasVariantVector*  args,
        ExportInterface::IDasVariantVector** out);
    DasResult HandleGetSessionInfo(
        ExportInterface::IDasVariantVector*  args,
        ExportInterface::IDasVariantVector** out);
};

class DasComponentFactoryImpl final
    : public PluginInterface::DasComponentFactoryImplBase<
          DasComponentFactoryImpl>
{
public:
    DasComponentFactoryImpl() = default;

    // IDasTypeInfo methods (必须手动实现)
    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL IsSupported(const DasGuid& component_iid) override;
    DAS_IMPL CreateInstance(
        const DasGuid&                   component_iid,
        PluginInterface::IDasComponent** pp_out_component) override;
};

/**
 * @brief 极简 IDasTaskComponent 实现 — 设置回显对象
 *
 * ApplySettingsChange 把请求 JSON 原样写入出参（跨 host 大消息链的靶），
 * Do 未实现（返回 DAS_E_NO_IMPLEMENTATION）。
 */
class EchoTaskComponent final
    : public PluginInterface::DasTaskComponentImplBase<EchoTaskComponent>
{
public:
    DAS_IMPL ApplySettingsChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json) override;

    DAS_IMPL Do(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map) override;
};

/**
 * @brief IDasTaskComponentHost feature 对象 — B 侧可递送物
 *
 * 经 CreateFeatureInterface([out]) 递出到调用方；CreateTaskComponent 对
 * 约定的测试 component guid 创建 EchoTaskComponent。guid 常量与
 * IpcTestPlugin1 侧约定保持一致（见 PluginImpl.cpp 匿名命名空间）。
 */
class TestTaskComponentHostFeature final
    : public PluginInterface::DasTaskComponentHostImplBase<
          TestTaskComponentHostFeature>
{
public:
    DAS_IMPL CreateTaskComponent(
        const DasGuid&                       component_guid,
        PluginInterface::IDasTaskComponent** pp_out_component) override;
};

/**
 * @brief IPC 测试插件2 - 实现 IDasPluginPackage
 *
 * 该插件提供：
 * - index=0: IDasComponentFactory 工厂（DasComponentFactoryImpl）
 * - index=1: IDasTaskComponentHost feature 对象（TestTaskComponentHostFeature）
 */
class IpcTestPlugin2 final
    : public PluginInterface::DasPluginPackageImplBase<IpcTestPlugin2>
{
    DasPtr<PluginInterface::IDasTaskComponentHost> task_component_host_;

public:
    DAS_IMPL EnumFeature(
        const size_t                       index,
        PluginInterface::DasPluginFeature* p_out_feature) override;

    DAS_IMPL CreateFeatureInterface(size_t index, IDasBase** pp_out_interface)
        override;

    // 存储加载器注入的 task component host（顺序契约：注入先于枚举）。
    DAS_IMPL SetTaskComponentHost(
        PluginInterface::IDasTaskComponentHost* p_host) override
    {
        task_component_host_ =
            DasPtr<PluginInterface::IDasTaskComponentHost>(p_host);
        return DAS_S_OK;
    }

    PluginInterface::IDasTaskComponentHost* GetTaskComponentHost() const
    {
        return task_component_host_.Get();
    }
};

DAS_NS_END

#endif
