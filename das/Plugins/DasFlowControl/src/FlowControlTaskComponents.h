#ifndef DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLTASKCOMPONENTS_H
#define DAS_PLUGINS_DASFLOWCONTROL_FLOWCONTROLTASKCOMPONENTS_H

#include <cassert>

#include <cpp_yyjson.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

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
        std::string_view                               kind,
        DasPtr<PluginInterface::IDasTaskComponentHost> host);

    DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
    DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

    DAS_IMPL ApplySettingsChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json) override;

    DAS_IMPL Do(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map) override;

private:
    DasResult DoRepositoryInvoke(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Pure signal routing: read `condition`, emit the `true`/`false` signal. The
    // runtime gates the downstream branch on the emitted signal (DAS-60 Stage 4).
    DasResult DoBranch(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Counted loop head: maintains `loop_index_` across Do() calls. Each call
    // writes the current `index` output and emits `continue`, or emits `break`
    // when the range is exhausted. The body is a separate main-graph node
    // re-activated by the runtime via the continue signal + back-edge.
    DasResult DoFor(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // Condition loop head: reads `condition` each call and emits
    // `continue`/`break`. No internal state.
    DasResult DoWhile(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    // φ-join: gated by in_true/in_false signals; copies whichever of
    // value_true/value_false the taken branch produced into `result`.
    DasResult DoMerge(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map);

    std::string                                    kind_;
    yyjson::value                                  settings_;
    DasPtr<PluginInterface::IDasTaskComponentHost> host_;

    // for-loop counter state. A fresh component instance is created per graph
    // execution (GraphRuntime::Configure), so this naturally resets per run;
    // it persists across the Do() calls of a single loop.
    int64_t loop_index_ = 0;
    bool    loop_started_ = false;
};

class DasFlowControlTaskComponentFactory final
    : public PluginInterface::DasTaskComponentFactoryImplBase<
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
