#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHBOUNDARYCOMPONENTS_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHBOUNDARYCOMPONENTS_H

#include <das/Core/GraphRuntime/Config.h>

#include <string_view>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// begin/end 边界组件的 component GUID 单一事实源。字面量与
// DasGraphTask.json 的 taskComponents.components 条目（键 +
// definition.componentGuid）同步——一致性由 DasGraphTaskTest 的 catalog
// 口面用例与预置节点编译用例背书（manifest 漂移即测试红）。后续的
// 输出侧导出排除与 UI 组件识别层从本头取值，禁止再造第二份字面量。
constexpr std::string_view GRAPH_BEGIN_COMPONENT_GUID =
    "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E";
constexpr std::string_view GRAPH_END_COMPONENT_GUID =
    "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D";

// DasGraphTask 插件 GUID（manifest "guid" 字段的镜像）：空图预置节点的
// componentRef.plugin_guid 数据源。Core 不加载插件，只能镜像此契约值。
constexpr std::string_view GRAPH_TASK_PLUGIN_GUID =
    "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHBOUNDARYCOMPONENTS_H
