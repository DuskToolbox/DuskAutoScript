#ifndef DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKBOUNDARYCOMPONENTS_H
#define DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKBOUNDARYCOMPONENTS_H

#include <das/DasGuidHolder.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>

#include <string>
#include <string_view>

namespace Das::Plugins::DasGraphTask
{
    class GraphTaskBoundaryComponent;
} // namespace Das::Plugins::DasGraphTask

// {3E8A5C71-D9B4-4F26-8E2A-6B0D4F9C1A58}
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    GraphTaskBoundaryComponent,
    0x3e8a5c71,
    0xd9b4,
    0x4f26,
    0x8e,
    0x2a,
    0x6b,
    0x0d,
    0x4f,
    0x9c,
    0x1a,
    0x58)

namespace Das::Plugins::DasGraphTask
{

    constexpr std::string_view GRAPH_BEGIN_KIND = "das.graph.begin";
    constexpr std::string_view GRAPH_END_KIND = "das.graph.end";

    // begin/end 边界组件：单类 kind 分派（manifest 双组件共用一个实现）。
    // Do 只做全类型同名 in→out 转写——值流不进组件代码，不感知哨兵绑定、
    // 不复制 $graph_input 常量；图的边界语义全部由编译层与预置承载。
    class GraphTaskBoundaryComponent final
        : public Das::PluginInterface::DasTaskComponentImplBase<
              GraphTaskBoundaryComponent>
    {
        std::string                                         kind_;
        DasPtr<Das::PluginInterface::IDasTaskComponentHost> host_;

    public:
        GraphTaskBoundaryComponent(
            std::string_view                                    kind,
            DasPtr<Das::PluginInterface::IDasTaskComponentHost> host);

        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL ApplySettingsChange(
            Das::ExportInterface::IDasJson*  p_request_json,
            Das::ExportInterface::IDasJson** pp_out_result_json) override;

        DAS_IMPL Do(
            Das::PluginInterface::IDasStopToken*       stop_token,
            Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
            Das::ExportInterface::IDasPortMap** pp_out_port_map) override;
    };

} // namespace Das::Plugins::DasGraphTask

#endif // DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKBOUNDARYCOMPONENTS_H
