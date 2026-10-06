#ifndef DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKBOUNDARYCOMPONENTS_H
#define DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKBOUNDARYCOMPONENTS_H

#include <das/DasGuidHolder.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTask.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>

#include <string>
#include <string_view>

namespace Das::Plugins::DasGraphTask
{
    class GraphTaskBoundaryComponent;
    class GraphTaskBoundaryHostTask;
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

// {6D2B9F4A-E7C1-43D8-9A5E-8B04C1F7D263}
DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    GraphTaskBoundaryHostTask,
    0x6d2b9f4a,
    0xe7c1,
    0x43d8,
    0x9a,
    0x5e,
    0x8b,
    0x04,
    0xc1,
    0xf7,
    0xd2,
    0x63)

namespace Das::Plugins::DasGraphTask
{

    constexpr std::string_view GRAPH_BEGIN_KIND = "das.graph.begin";
    constexpr std::string_view GRAPH_END_KIND = "das.graph.end";

    // begin/end 边界组件：单类 kind 分派（manifest 双组件共用一个实现）。
    // Do 只做全类型同名 in→out 转写——值流不进组件代码，不感知绑定记
    // 形态；图的边界语义全部由编译层与预置承载。
    class GraphTaskBoundaryComponent final
        : public Das::PluginInterface::DasTaskComponentImplBase<
              GraphTaskBoundaryComponent>
    {
        std::string                                            kind_;
        DasPtr<Das::PluginInterface::IDasTaskComponentCreator> host_;

    public:
        GraphTaskBoundaryComponent(
            std::string_view                                       kind,
            DasPtr<Das::PluginInterface::IDasTaskComponentCreator> host);

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

    // begin/end 组件 guid 的 authoring capability 托管载体（IDasTask 薄
    // 壳）：供 SchedulerService 既有能力注册面发现——TASK feature 经 QI
    // 取 GetGuid（=组件 guid）→ manifest tasks 描述符匹配 → capability
    // registry 建 record（键=组件 guid、authoring=本插件会话工厂、
    // executionComponent=同 guid）。既有 capability QI 链据此把
    // CreateSession(begin/end guid) 路由到 DasGraphTask 工厂（其 begin/end
    // 分支包装 Core 边界会话）。begin/end 是图内结构组件而非可调度任务：
    // Do 与排程查询一律结构化拒绝。类 GUID 仅供 ImplBase 模板实例化，不
    // 对外（GetGuid 返回组件 guid——与 GraphTaskBoundaryComponent 的
    // manifest 身份分派同款形态）。
    class GraphTaskBoundaryHostTask final
        : public Das::PluginInterface::DasTaskImplBase<
              GraphTaskBoundaryHostTask>
    {
        DasGuid component_guid_;

    public:
        explicit GraphTaskBoundaryHostTask(DasGuid component_guid);

        DAS_IMPL GetGuid(DasGuid* p_out_guid) override;
        DAS_IMPL GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;

        DAS_IMPL Do(
            Das::PluginInterface::IDasStopToken* stop_token,
            Das::ExportInterface::IDasJson*      p_environment_json,
            Das::ExportInterface::IDasJson*      p_task_settings_json) override;

        DAS_IMPL GetNextExecutionTime(
            Das::ExportInterface::IDasJson* p_task_settings_json,
            Das::ExportInterface::DasDate*  p_out_date) override;
    };

} // namespace Das::Plugins::DasGraphTask

#endif // DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKBOUNDARYCOMPONENTS_H
