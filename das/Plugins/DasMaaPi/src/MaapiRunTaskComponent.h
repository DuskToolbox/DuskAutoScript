#ifndef DAS_PLUGINS_DASMAAPI_MAAPIRUNTASKCOMPONENT_H
#define DAS_PLUGINS_DASMAAPI_MAAPIRUNTASKCOMPONENT_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentFactory.Implements.hpp>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiRunTaskComponent,
    0x69f20008,
    0x0000,
    0x4000,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x01);

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiRunTaskComponentFactory,
    0x69f20009,
    0x0000,
    0x4000,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x01);

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    // manifest（DasMaaPi.json）声明的执行组件：settings 为空、输入口
    // executionInput（object）、输出口 completedTasks/stopped/diagnostics、
    // 信号 succeeded/failed/cancelled。Do 是纯消费者——读输入口的 envelope
    // JSON 经共享执行内核（MaapiExecutionKernel）执行，不消费任何 settings
    // 键；envelope 由唯一编译器（MaapiAuthoringSession::Compile）产出，经
    // RepositoryInvoke 组件喂线（SetPortString("executionInput", ...)）。
    class MaapiRunTaskComponent final
        : public PluginInterface::DasTaskComponentImplBase<
              MaapiRunTaskComponent>
    {
    public:
        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL ApplySettingsChange(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;

        DAS_IMPL Do(
            PluginInterface::IDasStopToken*       stop_token,
            ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
            ExportInterface::IDasPortMap**        pp_out_port_map) override;
    };

    class MaapiRunTaskComponentFactory final
        : public PluginInterface::DasTaskComponentFactoryImplBase<
              MaapiRunTaskComponentFactory>
    {
        DasPtr<PluginInterface::IDasTaskComponentHost> host_;

    public:
        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL CreateComponent(
            const DasGuid&                       component_guid,
            PluginInterface::IDasTaskComponent** pp_out_component) override;

        DAS_IMPL SetTaskComponentHost(
            PluginInterface::IDasTaskComponentHost* p_host) override;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPIRUNTASKCOMPONENT_H
