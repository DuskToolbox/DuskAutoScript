#ifndef DAS_CORE_GRAPHRUNTIME_GRAPHBOUNDARYAUTHORINGSESSION_H
#define DAS_CORE_GRAPHRUNTIME_GRAPHBOUNDARYAUTHORINGSESSION_H

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/Config.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSession.Implements.hpp>

#include <cstdint>
#include <string>

// 边界组件（graph begin/end）的 authoring 会话 COM 类 GUID。接口本身复用
// IDasTaskAuthoringSession（零新接口/IID）；此 GUID 只标识实现类（ImplBase
// 的 GetGuid/DasIidOf<TImpl> 消费）。
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::GraphRuntime,
    GraphBoundaryAuthoringSession,
    0x6ba10001,
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

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// begin/end 边界组件的自有 authoring 会话——与 MaaPI 会话同构（状态域=
// 节点 settings 配置值；文档=纯投影现算，不存储）。
//
// 状态域：节点 settings JSON 的 boundaryPorts 键（数组 of {id,type}）。
// 方向不存储——由会话角色（begin=入 / end=出）固化推导。
//
// 文档投影（GetDocument）两面：
//   - formSequence.items = 口条目写通道（每口一条，条目字段 id 文本 +
//     type select，candidates=九词词表）；
//   - 根 ports section = 边界签名读通道（{id,type,direction}，direction
//     由角色恒定推导：begin 全 input / end 全 output）。
//
// 拒绝零突变：任何被拒变更不触碰 settings、不推进 revision。
class GraphBoundaryAuthoringSession final
    : public Das::PluginInterface::DasTaskAuthoringSessionImplBase<
          GraphBoundaryAuthoringSession>
{
public:
    // 会话角色：由 create 出口的 component_guid 参数决定（方向固化的信
    // 任基础）。kNone = 非 begin/end guid（create 出口据此拒绝）。
    enum class BoundaryRole
    {
        kNone,
        kBegin,
        kEnd,
    };

    // guid → 角色解析（fail-closed）。比对常量从
    // GraphBoundaryComponents.h 的 string_view 单一事实源静态解析一次成
    // DasGuid 形态（不产生第二份字面量事实源）。
    static BoundaryRole RoleOfGuid(const DasGuid& component_guid);

    // context_json：该节点的 settings 对象（boundaryPorts 键 + 其它配置
    // 值原样保留）；顶层 revision 键（若为整数）初始化会话 revision，其
    // 余键不解释。空串/解析失败 → 空 settings 起步（engaged-only）。
    GraphBoundaryAuthoringSession(
        BoundaryRole       role,
        const std::string& context_json);

    // --- IDasTaskAuthoringSession 三件套 ---

    DAS_IMPL GetDocument(
        Das::ExportInterface::IDasJson*  p_request_json,
        Das::ExportInterface::IDasJson** pp_out_document_json) override;

    DAS_IMPL ApplyChange(
        Das::ExportInterface::IDasJson*  p_request_json,
        Das::ExportInterface::IDasJson** pp_out_result_json) override;

    DAS_IMPL Compile(
        Das::ExportInterface::IDasJson*  p_request_json,
        Das::ExportInterface::IDasJson** pp_out_result_json) override;

private:
    BoundaryRole role_ = BoundaryRole::kNone;
    // 节点 settings 对象（全键保留；boundaryPorts 是本会话的编辑域）。
    // 文档与 acceptedProperties 均从此对象现算投影。
    yyjson::value settings_{Das::Utils::MakeYyjsonObject()};
    int64_t       revision_ = 0;
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_GRAPHBOUNDARYAUTHORINGSESSION_H
