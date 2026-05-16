#ifndef DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLTASKCOMPONENTS_H
#define DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLTASKCOMPONENTS_H

#include <cassert>

#include <cpp_yyjson.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentHostAware.Implements.hpp>

#include <string>
#include <string_view>

// {523148A6-E5BD-4B32-B8C1-C9820214014C}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    DasFlowControlTaskComponent,
    0x523148a6,
    0xe5bd,
    0x4b32,
    0xb8,
    0xc1,
    0xc9,
    0x82,
    0x02,
    0x14,
    0x01,
    0x4c);

// {2C6DFCFA-F8B6-4272-8F28-1D31738BD0D4}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    DasFlowControlTaskComponentFactory,
    0x2c6dfcfa,
    0xf8b6,
    0x4272,
    0x8f,
    0x28,
    0x1d,
    0x31,
    0x73,
    0x8b,
    0xd0,
    0xd4);

DAS_NS_BEGIN

class DasFlowControlTaskComponent final
    : public PluginInterface::DasTaskComponentImplBase<
          DasFlowControlTaskComponent>
{
public:
    DasFlowControlTaskComponent(
        std::string_view kind,
        DasPtr<PluginInterface::IDasTaskComponentHost> host);

    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL ApplySettingsChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json) override;

    DAS_IMPL Do(
        PluginInterface::IDasStopToken* stop_token,
        ExportInterface::IDasJson*      p_environment_json,
        ExportInterface::IDasJson*      p_settings_json,
        ExportInterface::IDasJson*      p_input_json,
        ExportInterface::IDasJson**     pp_out_result_json) override;

private:
    DasResult DoRepositoryInvoke(
        PluginInterface::IDasStopToken* stop_token,
        ExportInterface::IDasJson*      p_environment_json,
        ExportInterface::IDasJson*      p_settings_json,
        ExportInterface::IDasJson*      p_input_json,
        ExportInterface::IDasJson**     pp_out_result_json);

    std::string                                      kind_;
    yyjson::value                                    settings_;
    DasPtr<PluginInterface::IDasTaskComponentHost>   host_;
};

class DasFlowControlTaskComponentFactory final
    : public PluginInterface::DasTaskComponentHostAwareImplBase<
          DasFlowControlTaskComponentFactory>
{
public:
    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL CreateComponent(
        const DasGuid&                       component_guid,
        PluginInterface::IDasTaskComponent** pp_out_component) override;

    DAS_IMPL SetTaskComponentHost(
        PluginInterface::IDasTaskComponentHost* p_host) override;

private:
    DasPtr<PluginInterface::IDasTaskComponentHost> host_;
};

DAS_NS_END

#endif // DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLTASKCOMPONENTS_H
