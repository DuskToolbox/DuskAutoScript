#ifndef DAS_PLUGINS_IPCTESTPLUGIN2_PLUGINIMPL_H
#define DAS_PLUGINS_IPCTESTPLUGIN2_PLUGINIMPL_H

#include <cstdint>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasComponentFactory.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>

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

DAS_NS_BEGIN

class DasComponentImpl final
    : public PluginInterface::DasComponentImplBase<DasComponentImpl>
{
public:
    explicit DasComponentImpl(uint16_t session_id);

    // IDasTypeInfo methods (必须手动实现)
    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL Dispatch(
        IDasReadOnlyString*                  p_function_name,
        ExportInterface::IDasVariantVector*  p_arguments,
        ExportInterface::IDasVariantVector** pp_out_result) override;

private:
    uint16_t session_id_;

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
    explicit DasComponentFactoryImpl(uint16_t session_id);

    // IDasTypeInfo methods (必须手动实现)
    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL IsSupported(const DasGuid& component_iid) override;
    DAS_IMPL CreateInstance(
        const DasGuid&                   component_iid,
        PluginInterface::IDasComponent** pp_out_component) override;

private:
    uint16_t session_id_;
};

class IpcTestPlugin2 final
    : public PluginInterface::DasPluginPackageImplBase<IpcTestPlugin2>
{
public:
    void SetSessionId(uint16_t session_id);

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

#endif
