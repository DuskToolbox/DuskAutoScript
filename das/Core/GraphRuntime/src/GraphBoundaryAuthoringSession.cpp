#include "GraphBoundaryAuthoringSession.h"

#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/GraphRuntime/AuthoringDocContract.h>
#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/TaskComponentPorts.h>

#include <array>
#include <cassert>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

namespace
{
    // 边界口 value_type 词表（九词）。上游两处白名单
    // （AuthoringDocContract.cpp / TaskAuthoringContract.cpp 的
    // NODE_PORT_VALUE_TYPES）均位于匿名命名空间不可达——本镜像与它们同
    // 语义 lockstep，改词表时三处同步演化。
    constexpr std::array<std::string_view, 9> kBoundaryPortValueTypes{
        "bool",
        "int",
        "int64",
        "float",
        "double",
        "string",
        "image",
        "base",
        "component"};

    constexpr std::size_t kMaxPortIdUtf8Bytes = 100;

    // 口 id 纪律（非空 / UTF-8 字节数 ≤100 / 不以 "$" 开头——哨兵源命名
    // 空间保留）。与 AuthoringDocContract.cpp 的 NodePortNameViolation 同
    // 语义 lockstep（该函数在匿名命名空间不可达，镜像并同步演化）。
    std::optional<std::string> PortIdViolation(std::string_view id)
    {
        if (id.empty())
        {
            return std::string("port id required");
        }
        if (id.size() > kMaxPortIdUtf8Bytes)
        {
            return std::string("port id must be at most 100 UTF-8 bytes");
        }
        if (id.front() == '$')
        {
            return std::string("port id must not start with '$' (reserved)");
        }
        return std::nullopt;
    }

    bool ValueTypeAllowed(std::string_view value_type)
    {
        for (const auto allowed : kBoundaryPortValueTypes)
        {
            if (value_type == allowed)
            {
                return true;
            }
        }
        return false;
    }

    // 边界组件 guid 的 DasGuid 形态常量——从 GraphBoundaryComponents.h 的
    // string_view 单一事实源静态解析一次（不产生第二份字面量事实源）。
    const DasGuid& BeginComponentGuidStorage()
    {
        static const DasGuid guid =
            Das::Core::ForeignInterfaceHost::MakeDasGuid(
                Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID);
        return guid;
    }

    const DasGuid& EndComponentGuidStorage()
    {
        static const DasGuid guid =
            Das::Core::ForeignInterfaceHost::MakeDasGuid(
                Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID);
        return guid;
    }

    // chrome 文案与字段 label（中文常量）：Core GraphRuntime 现无 i18n 消
    // 费先例（IDasI18n 通道未接入本模块），文案以常量落地；i18n 接入时迁
    // 移到翻译表。
    constexpr std::string_view kBeginListTitle = "输入端口";
    constexpr std::string_view kBeginAddButton = "＋ 添加端口";
    constexpr std::string_view kBeginEmptyState = "尚未声明输入端口";
    constexpr std::string_view kEndListTitle = "输出端口";
    constexpr std::string_view kEndAddButton = "＋ 添加端口";
    constexpr std::string_view kEndEmptyState = "尚未声明输出端口";
    constexpr std::string_view kPortIdFieldLabel = "端口 ID";
    constexpr std::string_view kPortTypeFieldLabel = "类型";

    // 边界口条目（settings.boundaryPorts 的解析形态）。方向不存储——由会
    // 话角色固化推导。
    struct BoundaryPortEntry
    {
        std::string id;
        std::string type;
    };

    // settings 对象的 boundaryPorts 键名。
    constexpr std::string_view kBoundaryPortsKey = "boundaryPorts";

    yyjson::value JsonStr(std::string_view text)
    {
        return yyjson::value(std::string(text));
    }

    // 读取 settings 的 boundaryPorts 数组（fail-closed 逐条）：键缺席 =
    // 空起步态（engaged-only）；非数组 / 条目形状违规 / id 纪律违规 /
    // type 超九词 / id 重复 → 该键整体降级为零口 + WARNING（faces 通道同
    // 款降级语义——降级到「无口」而非「全口」，保证投影恒过契约校验）。
    std::vector<BoundaryPortEntry> LoadBoundaryPorts(
        const yyjson::value& settings)
    {
        std::vector<BoundaryPortEntry> ports;
        auto                           settings_obj = settings.as_object();
        if (!settings_obj || !settings_obj->contains(kBoundaryPortsKey))
        {
            return ports;
        }
        const auto degrade = [](std::string_view reason)
        {
            DAS_CORE_LOG_WARN(
                "boundaryPorts settings key rejected ({}); the boundary "
                "session continues with no ports",
                reason);
        };
        auto arr =
            (*settings_obj)[std::string_view(kBoundaryPortsKey)].as_array();
        if (!arr)
        {
            degrade("value is not an array");
            return ports;
        }
        ports.reserve(arr->size());
        for (const auto& entry_value : *arr)
        {
            auto entry_obj = entry_value.as_object();
            if (!entry_obj)
            {
                degrade("entry is not an object");
                return {};
            }
            BoundaryPortEntry entry;
            if (auto id = (*entry_obj)[std::string_view("id")].as_string())
            {
                entry.id = std::string(*id);
            }
            else
            {
                degrade("entry id is missing or not a string");
                return {};
            }
            if (auto type = (*entry_obj)[std::string_view("type")].as_string())
            {
                entry.type = std::string(*type);
            }
            else
            {
                degrade("entry type is missing or not a string");
                return {};
            }
            if (PortIdViolation(entry.id).has_value())
            {
                degrade("entry id violates the port id discipline");
                return {};
            }
            if (!ValueTypeAllowed(entry.type))
            {
                degrade("entry type is outside the nine-word vocabulary");
                return {};
            }
            for (const auto& existing : ports)
            {
                if (existing.id == entry.id)
                {
                    degrade("duplicate entry id");
                    return {};
                }
            }
            ports.push_back(std::move(entry));
        }
        return ports;
    }

    // 把解析形态写回 settings 的 boundaryPorts 键（其余键原样保留）。
    void WriteBoundaryPorts(
        yyjson::value&                        settings,
        const std::vector<BoundaryPortEntry>& ports)
    {
        auto settings_obj = settings.as_object();
        if (!settings_obj)
        {
            return;
        }
        yyjson::value array_value(Das::Utils::MakeYyjsonArray());
        auto          arr = array_value.as_array();
        for (const auto& port : ports)
        {
            yyjson::value entry_value(Das::Utils::MakeYyjsonObject());
            auto          entry_obj = entry_value.as_object();
            (*entry_obj)[std::string_view("id")] = JsonStr(port.id);
            (*entry_obj)[std::string_view("type")] = JsonStr(port.type);
            arr->emplace_back(std::move(entry_value));
        }
        (*settings_obj)[kBoundaryPortsKey] = std::move(array_value);
    }

    // settings 的 acceptedProperties 形态（其它键原样透传）。
    yyjson::value CloneSettings(const yyjson::value& settings)
    {
        return Das::Utils::CloneYyjsonValue(settings);
    }

    // 条目字段投影：id 文本字段（必填）+ type select 字段
    // （candidates=九词词表）。
    yyjson::value BuildItemFields(const BoundaryPortEntry& port)
    {
        yyjson::value fields_value(Das::Utils::MakeYyjsonArray());
        auto          fields_arr = fields_value.as_array();

        yyjson::value id_field(Das::Utils::MakeYyjsonObject());
        {
            auto id_obj = id_field.as_object();
            (*id_obj)[std::string_view("name")] = JsonStr("id");
            (*id_obj)[std::string_view("label")] = JsonStr(kPortIdFieldLabel);
            (*id_obj)[std::string_view("valueType")] = 4; // string
            (*id_obj)[std::string_view("inputType")] = 0; // plain
            (*id_obj)[std::string_view("value")] = JsonStr(port.id);
            (*id_obj)[std::string_view("required")] = true;
        }
        fields_arr->emplace_back(std::move(id_field));

        yyjson::value type_field(Das::Utils::MakeYyjsonObject());
        {
            auto type_obj = type_field.as_object();
            (*type_obj)[std::string_view("name")] = JsonStr("type");
            (*type_obj)[std::string_view("label")] =
                JsonStr(kPortTypeFieldLabel);
            (*type_obj)[std::string_view("valueType")] = 4; // string
            (*type_obj)[std::string_view("inputType")] = 1; // select
            (*type_obj)[std::string_view("value")] = JsonStr(port.type);
            yyjson::value candidates_value(Das::Utils::MakeYyjsonArray());
            auto          candidates_arr = candidates_value.as_array();
            for (const auto word : kBoundaryPortValueTypes)
            {
                yyjson::value candidate(Das::Utils::MakeYyjsonObject());
                auto          candidate_obj = candidate.as_object();
                (*candidate_obj)[std::string_view("value")] = JsonStr(word);
                (*candidate_obj)[std::string_view("label")] = JsonStr(word);
                candidates_arr->emplace_back(std::move(candidate));
            }
            (*type_obj)[std::string_view("candidates")] =
                std::move(candidates_value);
        }
        fields_arr->emplace_back(std::move(type_field));
        return fields_value;
    }

    // 根 ports section 条目（边界签名读通道）：{id, type, direction}——
    // direction 由角色恒定推导（begin 全 input / end 全 output）。
    yyjson::value BuildPortsSection(
        const std::vector<BoundaryPortEntry>&       ports,
        GraphBoundaryAuthoringSession::BoundaryRole role)
    {
        const std::string_view direction =
            role == GraphBoundaryAuthoringSession::BoundaryRole::kBegin
                ? std::string_view("input")
                : std::string_view("output");
        yyjson::value ports_value(Das::Utils::MakeYyjsonArray());
        auto          ports_arr = ports_value.as_array();
        for (const auto& port : ports)
        {
            yyjson::value entry(Das::Utils::MakeYyjsonObject());
            auto          entry_obj = entry.as_object();
            (*entry_obj)[std::string_view("id")] = JsonStr(port.id);
            (*entry_obj)[std::string_view("type")] = JsonStr(port.type);
            (*entry_obj)[std::string_view("direction")] = JsonStr(direction);
            ports_arr->emplace_back(std::move(entry));
        }
        return ports_value;
    }

    // 文档投影（纯现算，不存储）：formSequence 视图（items 写通道 +
    // chrome 三键）+ 根 ports section（读通道）+ actionCatalog（本边界组
    // 件自身——UI 列表「添加」入口的唯一可添加动作）。types section 本期
    // 不投影（九词内建即够，注册类型随类型注册表通道再启）。
    yyjson::value BuildDocumentValue(
        GraphBoundaryAuthoringSession::BoundaryRole role,
        const std::vector<BoundaryPortEntry>&       ports,
        int64_t                                     revision)
    {
        const bool is_begin =
            role == GraphBoundaryAuthoringSession::BoundaryRole::kBegin;
        const std::string_view component_guid =
            is_begin ? Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID
                     : Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID;
        const std::string_view component_kind =
            is_begin ? std::string_view("das.graph.begin")
                     : std::string_view("das.graph.end");

        yyjson::value document(Das::Utils::MakeYyjsonObject());
        auto          doc_obj = document.as_object();
        (*doc_obj)[std::string_view("contractVersion")] = 1;
        (*doc_obj)[std::string_view("viewKind")] = JsonStr("formSequence");
        (*doc_obj)[std::string_view("revision")] = revision;

        yyjson::value form_sequence(Das::Utils::MakeYyjsonObject());
        {
            auto          fs_obj = form_sequence.as_object();
            yyjson::value items_value(Das::Utils::MakeYyjsonArray());
            auto          items_arr = items_value.as_array();
            for (const auto& port : ports)
            {
                yyjson::value item(Das::Utils::MakeYyjsonObject());
                auto          item_obj = item.as_object();
                (*item_obj)[std::string_view("id")] = JsonStr(port.id);
                (*item_obj)[std::string_view("fields")] = BuildItemFields(port);
                items_arr->emplace_back(std::move(item));
            }
            (*fs_obj)[std::string_view("items")] = std::move(items_value);
            (*fs_obj)[std::string_view("listTitle")] =
                JsonStr(is_begin ? kBeginListTitle : kEndListTitle);
            (*fs_obj)[std::string_view("addButtonLabel")] =
                JsonStr(is_begin ? kBeginAddButton : kEndAddButton);
            (*fs_obj)[std::string_view("emptyStateHint")] =
                JsonStr(is_begin ? kBeginEmptyState : kEndEmptyState);
        }
        (*doc_obj)[std::string_view("formSequence")] = std::move(form_sequence);

        (*doc_obj)[std::string_view("ports")] = BuildPortsSection(ports, role);

        yyjson::value catalog(Das::Utils::MakeYyjsonObject());
        {
            auto          catalog_obj = catalog.as_object();
            yyjson::value entries_value(Das::Utils::MakeYyjsonArray());
            auto          entries_arr = entries_value.as_array();
            yyjson::value entry(Das::Utils::MakeYyjsonObject());
            auto          entry_obj = entry.as_object();
            (*entry_obj)[std::string_view("actionId")] =
                JsonStr(component_guid);
            (*entry_obj)[std::string_view("label")] = JsonStr(component_kind);
            entries_arr->emplace_back(std::move(entry));
            (*catalog_obj)[std::string_view("entries")] =
                std::move(entries_value);
        }
        (*doc_obj)[std::string_view("actionCatalog")] = std::move(catalog);
        return document;
    }

    // 诊断数组序列化（severity/message 必带；code/path engaged-only——
    // 与文档协议诊断 wire 形状一致）。
    yyjson::value BuildDiagnosticsValue(
        const std::vector<Contract::Diagnostic>& diagnostics)
    {
        yyjson::value diagnostics_value(Das::Utils::MakeYyjsonArray());
        auto          arr = diagnostics_value.as_array();
        for (const auto& diagnostic : diagnostics)
        {
            yyjson::value item(Das::Utils::MakeYyjsonObject());
            auto          item_obj = item.as_object();
            (*item_obj)[std::string_view("severity")] =
                JsonStr(diagnostic.severity);
            (*item_obj)[std::string_view("message")] =
                JsonStr(diagnostic.message);
            if (diagnostic.code)
            {
                (*item_obj)[std::string_view("code")] = *diagnostic.code;
            }
            if (diagnostic.path)
            {
                (*item_obj)[std::string_view("path")] =
                    JsonStr(*diagnostic.path);
            }
            arr->emplace_back(std::move(item));
        }
        return diagnostics_value;
    }

    Contract::Diagnostic MakeErrorDiagnostic(std::string message)
    {
        Contract::Diagnostic diagnostic;
        diagnostic.severity = "error";
        diagnostic.message = std::move(message);
        return diagnostic;
    }

    // 严格 Apply 请求外层 {baseRevision, kind, payload}（MaaPI 会话同款
    // 严格形状：键集恰好三键、baseRevision 整数、kind 字符串、payload
    // 对象）。
    struct ApplyRequest
    {
        int64_t       base_revision = 0;
        std::string   kind;
        yyjson::value payload{Das::Utils::MakeYyjsonObject()};
    };

    bool ParseApplyRequest(
        const yyjson::value&               request,
        ApplyRequest&                      out,
        std::vector<Contract::Diagnostic>& diagnostics)
    {
        auto obj = request.as_object();
        if (!obj)
        {
            diagnostics.push_back(
                MakeErrorDiagnostic("Apply request must be a JSON object"));
            return false;
        }
        std::set<std::string> actual;
        for (const auto& [key, value] : *obj)
        {
            std::ignore = value;
            actual.emplace(std::string(key));
        }
        if (actual != std::set<std::string>{"baseRevision", "kind", "payload"})
        {
            diagnostics.push_back(MakeErrorDiagnostic(
                "Apply request must have exactly "
                "{baseRevision, kind, payload}"));
            return false;
        }
        if (!(*obj)[std::string_view("baseRevision")].is_int())
        {
            diagnostics.push_back(
                MakeErrorDiagnostic("baseRevision must be integer"));
            return false;
        }
        out.base_revision =
            (*obj)[std::string_view("baseRevision")].as_int().value_or(0);
        auto kind = (*obj)[std::string_view("kind")].as_string();
        if (!kind)
        {
            diagnostics.push_back(MakeErrorDiagnostic("kind must be string"));
            return false;
        }
        out.kind = std::string(*kind);
        out.payload =
            Das::Utils::CloneYyjsonValue((*obj)[std::string_view("payload")]);
        if (!out.payload.is_object())
        {
            diagnostics.push_back(
                MakeErrorDiagnostic("payload must be a JSON object"));
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------
    // 变更词表分派（全部既有 kind 字符串——会话把 item 家族与
    // addNodePort 家族解释成口操作，与 MaaPI 把 item 家族解释成任务操作
    // 同构；payload 形状遵循文档协议 change 校验层的锁形——上游管线在
    // 会话分派前已校验，此处再作语义校验（纪律/方向门/唯一性））。
    // ------------------------------------------------------------------

    Contract::Diagnostic MakeInfoDiagnostic(std::string message)
    {
        Contract::Diagnostic diagnostic;
        diagnostic.severity = "info";
        diagnostic.message = std::move(message);
        return diagnostic;
    }

    std::optional<std::string> PayloadString(
        const yyjson::value& payload,
        std::string_view     key)
    {
        auto obj = payload.as_object();
        if (!obj || !obj->contains(key))
        {
            return std::nullopt;
        }
        auto value = (*obj)[key].as_string();
        if (!value)
        {
            return std::nullopt;
        }
        return std::string(*value);
    }

    std::optional<int64_t> PayloadInt(
        const yyjson::value& payload,
        std::string_view     key)
    {
        auto obj = payload.as_object();
        if (!obj || !obj->contains(key))
        {
            return std::nullopt;
        }
        return (*obj)[key].as_int();
    }

    std::string_view ComponentGuidTextOfRole(
        GraphBoundaryAuthoringSession::BoundaryRole role)
    {
        return role == GraphBoundaryAuthoringSession::BoundaryRole::kBegin
                   ? Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID
                   : Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID;
    }

    std::string_view RoleDirectionOf(
        GraphBoundaryAuthoringSession::BoundaryRole role)
    {
        return role == GraphBoundaryAuthoringSession::BoundaryRole::kBegin
                   ? std::string_view("input")
                   : std::string_view("output");
    }

    // 条目 id 分配（照稳定口 id 分配模式）："p_" 前缀计数器 + 现有 id
    // 扫描防撞，首口 p_1。条目 id 即口 id（寻址键），文档内必须唯一。
    std::string AllocateBoundaryPortId(
        const std::vector<BoundaryPortEntry>& ports)
    {
        std::set<std::string> existing;
        for (const auto& port : ports)
        {
            existing.insert(port.id);
        }
        std::size_t counter = existing.size() + 1;
        std::string candidate_id;
        do
        {
            candidate_id = "p_" + std::to_string(counter);
            ++counter;
        } while (existing.count(candidate_id) != 0);
        return candidate_id;
    }

    std::size_t FindPortIndex(
        const std::vector<BoundaryPortEntry>& ports,
        std::string_view                      id)
    {
        for (std::size_t index = 0; index < ports.size(); ++index)
        {
            if (ports[index].id == id)
            {
                return index;
            }
        }
        return ports.size();
    }

    // 分派单条变更。返回 true = candidate 已变更（调用方提交并递增
    // revision）；false = 拒绝（diagnostics 已携带 error 条目，candidate
    // 零突变）。nodeId 类寻址字段在本会话语境不参与定位——会话构造即绑
    // 定单一节点的 settings（guid+context），口列表唯一，nodeId 只作管线
    // 侧上下文透传。
    bool DispatchBoundaryChange(
        GraphBoundaryAuthoringSession::BoundaryRole role,
        const std::string&                          kind,
        const yyjson::value&                        payload,
        std::vector<BoundaryPortEntry>&             candidate,
        std::vector<Contract::Diagnostic>&          diagnostics)
    {
        const auto reject = [&diagnostics](std::string message)
        {
            diagnostics.push_back(MakeErrorDiagnostic(std::move(message)));
            return false;
        };

        if (kind == "addFormSequenceItem")
        {
            // actionId 通道：本边界文档唯一可添加的「动作」是边界组件自
            // 身（文档 actionCatalog 只含该条目）——其它 actionId 拒绝。
            const auto action_id = PayloadString(payload, "actionId");
            if (!action_id)
            {
                return reject("addFormSequenceItem.actionId required");
            }
            if (*action_id != ComponentGuidTextOfRole(role))
            {
                return reject(
                    "addFormSequenceItem.actionId must be the boundary "
                    "component itself: "
                    + std::string(ComponentGuidTextOfRole(role)));
            }
            BoundaryPortEntry entry;
            entry.id = AllocateBoundaryPortId(candidate);
            entry.type = "string";
            if (const auto insertion_index =
                    PayloadInt(payload, "insertionIndex"))
            {
                if (*insertion_index < 0
                    || static_cast<std::size_t>(*insertion_index)
                           > candidate.size())
                {
                    return reject(
                        "addFormSequenceItem.insertionIndex out of range");
                }
                candidate.insert(
                    candidate.begin()
                        + static_cast<std::ptrdiff_t>(*insertion_index),
                    std::move(entry));
            }
            else
            {
                candidate.push_back(std::move(entry));
            }
            return true;
        }

        if (kind == "removeFormSequenceItem")
        {
            const auto id = PayloadString(payload, "formSequenceItemId");
            if (!id)
            {
                return reject(
                    "removeFormSequenceItem.formSequenceItemId required");
            }
            const auto index = FindPortIndex(candidate, *id);
            if (index == candidate.size())
            {
                return reject("boundary port not found: " + *id);
            }
            candidate.erase(
                candidate.begin() + static_cast<std::ptrdiff_t>(index));
            // 被删口 id 随 diagnostics 回传（宿主管线据此级联删边；会话
            // 自身不持图边）。
            diagnostics.push_back(
                MakeInfoDiagnostic("boundary port removed: " + *id));
            return true;
        }

        if (kind == "moveFormSequenceItem")
        {
            const auto id = PayloadString(payload, "formSequenceItemId");
            if (!id)
            {
                return reject(
                    "moveFormSequenceItem.formSequenceItemId required");
            }
            const auto destination = PayloadInt(payload, "destinationIndex");
            if (!destination)
            {
                return reject("moveFormSequenceItem.destinationIndex required");
            }
            const auto index = FindPortIndex(candidate, *id);
            if (index == candidate.size())
            {
                return reject("boundary port not found: " + *id);
            }
            if (*destination < 0
                || static_cast<std::size_t>(*destination) >= candidate.size())
            {
                return reject(
                    "moveFormSequenceItem.destinationIndex out of range");
            }
            BoundaryPortEntry entry = std::move(candidate[index]);
            candidate.erase(
                candidate.begin() + static_cast<std::ptrdiff_t>(index));
            candidate.insert(
                candidate.begin() + static_cast<std::ptrdiff_t>(*destination),
                std::move(entry));
            return true;
        }

        if (kind == "addNodePort")
        {
            const auto name = PayloadString(payload, "name");
            if (!name)
            {
                return reject("addNodePort.name required");
            }
            const auto direction = PayloadString(payload, "direction");
            if (!direction)
            {
                return reject("addNodePort.direction required");
            }
            // 方向门：方向由会话角色固化（begin 只入 / end 只出）——与
            // 角色相反的签名方向拒绝，任何变更路径都产不出反向口。
            if (*direction != RoleDirectionOf(role))
            {
                return reject(
                    "addNodePort.direction mismatch: Begin accepts input "
                    "ports, End accepts output ports");
            }
            const auto value_type = PayloadString(payload, "valueType");
            if (!value_type)
            {
                return reject("addNodePort.valueType required");
            }
            if (!ValueTypeAllowed(*value_type))
            {
                return reject(
                    "addNodePort.valueType must be one of bool/int/int64/"
                    "float/double/string/image/base/component");
            }
            if (const auto violation = PortIdViolation(*name);
                violation.has_value())
            {
                return reject("addNodePort.name " + *violation);
            }
            if (FindPortIndex(candidate, *name) != candidate.size())
            {
                return reject(
                    "addNodePort.name duplicates an existing port id: "
                    + *name);
            }
            // 口 id 与口名在本文档是同一通道（条目 {id,type} 单 id 形
            // 态）——portId 在场时必须与 name 一致，分歧即拒绝。
            if (const auto port_id = PayloadString(payload, "portId");
                port_id.has_value() && *port_id != *name)
            {
                return reject(
                    "addNodePort.portId must equal name for boundary ports "
                    "(the port id and its name are one)");
            }
            candidate.push_back(BoundaryPortEntry{*name, *value_type});
            return true;
        }

        if (kind == "removeNodePort")
        {
            const auto port_id = PayloadString(payload, "portId");
            if (!port_id)
            {
                return reject("removeNodePort.portId required");
            }
            const auto index = FindPortIndex(candidate, *port_id);
            if (index == candidate.size())
            {
                return reject("boundary port not found: " + *port_id);
            }
            candidate.erase(
                candidate.begin() + static_cast<std::ptrdiff_t>(index));
            diagnostics.push_back(
                MakeInfoDiagnostic("boundary port removed: " + *port_id));
            return true;
        }

        if (kind == "renameNodePort")
        {
            const auto port_id = PayloadString(payload, "portId");
            if (!port_id)
            {
                return reject("renameNodePort.portId required");
            }
            const auto name = PayloadString(payload, "name");
            if (!name)
            {
                return reject("renameNodePort.name required");
            }
            const auto index = FindPortIndex(candidate, *port_id);
            if (index == candidate.size())
            {
                return reject("boundary port not found: " + *port_id);
            }
            if (const auto violation = PortIdViolation(*name);
                violation.has_value())
            {
                return reject("renameNodePort.name " + *violation);
            }
            for (std::size_t other = 0; other < candidate.size(); ++other)
            {
                if (other != index && candidate[other].id == *name)
                {
                    return reject(
                        "renameNodePort.name duplicates an existing port "
                        "id: "
                        + *name);
                }
            }
            candidate[index].id = *name;
            return true;
        }

        if (kind == "setValue")
        {
            auto obj = payload.as_object();
            if (!obj || !obj->contains(std::string_view("target")))
            {
                return reject("setValue.target required");
            }
            auto target_obj = (*obj)[std::string_view("target")].as_object();
            if (!target_obj)
            {
                return reject("setValue.target must be an object");
            }
            auto target_kind =
                (*target_obj)[std::string_view("kind")].as_string();
            if (!target_kind
                || *target_kind != std::string_view("formSequenceItem"))
            {
                return reject(
                    "setValue.target.kind must be formSequenceItem in the "
                    "boundary document");
            }
            auto target_id = (*target_obj)[std::string_view("id")].as_string();
            if (!target_id || target_id->empty())
            {
                return reject("setValue.target.id required");
            }
            const auto field_name = PayloadString(payload, "fieldName");
            if (!field_name)
            {
                return reject("setValue.fieldName required");
            }
            if (!obj->contains(std::string_view("value")))
            {
                return reject("setValue.value required");
            }
            const auto index = FindPortIndex(candidate, *target_id);
            if (index == candidate.size())
            {
                return reject(
                    "boundary port not found: " + std::string(*target_id));
            }
            const auto& value = (*obj)[std::string_view("value")];
            if (*field_name == "id")
            {
                auto new_id = value.as_string();
                if (!new_id)
                {
                    return reject(
                        "setValue.value must be a string for the id field");
                }
                if (const auto violation = PortIdViolation(*new_id);
                    violation.has_value())
                {
                    return reject("setValue.value " + *violation);
                }
                for (std::size_t other = 0; other < candidate.size(); ++other)
                {
                    if (other != index && candidate[other].id == *new_id)
                    {
                        return reject(
                            "setValue.value duplicates an existing port "
                            "id: "
                            + std::string(*new_id));
                    }
                }
                candidate[index].id = std::string(*new_id);
                return true;
            }
            if (*field_name == "type")
            {
                auto new_type = value.as_string();
                if (!new_type)
                {
                    return reject(
                        "setValue.value must be a string for the type "
                        "field");
                }
                if (!ValueTypeAllowed(*new_type))
                {
                    return reject(
                        "setValue.value must be one of bool/int/int64/"
                        "float/double/string/image/base/component");
                }
                candidate[index].type = std::string(*new_type);
                return true;
            }
            return reject(
                "setValue.fieldName must be id or type for boundary port "
                "items");
        }

        // 未知/图专属 kind（connectGraphPorts、convertView、
        // invokeCommand 等）fail-closed 拒绝。
        return reject(
            "Unsupported change kind for the boundary document: " + kind);
    }
} // namespace

GraphBoundaryAuthoringSession::BoundaryRole
GraphBoundaryAuthoringSession::RoleOfGuid(const DasGuid& component_guid)
{
    if (component_guid == BeginComponentGuidStorage())
    {
        return BoundaryRole::kBegin;
    }
    if (component_guid == EndComponentGuidStorage())
    {
        return BoundaryRole::kEnd;
    }
    return BoundaryRole::kNone;
}

GraphBoundaryAuthoringSession::GraphBoundaryAuthoringSession(
    BoundaryRole       role,
    const std::string& context_json)
    : role_(role)
{
    if (context_json.empty())
    {
        return;
    }
    yyjson::value root;
    try
    {
        root = yyjson::read(context_json);
    }
    catch (...)
    {
        DAS_CORE_LOG_WARN(
            "boundary authoring session context is not valid JSON; the "
            "session starts from empty settings");
        return;
    }
    auto obj = root.as_object();
    if (!obj)
    {
        DAS_CORE_LOG_WARN(
            "boundary authoring session context is not a JSON object; the "
            "session starts from empty settings");
        return;
    }
    // 顶层 revision 键是管线装配的会话权威版本（不属 settings 本体）——
    // 读出后不进 settings，acceptedProperties 不回显它；其余键（含
    // boundaryPorts 与任何管线透传键）原样保留。
    yyjson::value settings(Das::Utils::MakeYyjsonObject());
    auto          settings_obj = settings.as_object();
    for (const auto& [key, value] : *obj)
    {
        if (key == std::string_view("revision"))
        {
            if (auto revision = value.as_int())
            {
                revision_ = *revision;
            }
            continue;
        }
        (*settings_obj)[std::string_view(key)] =
            Das::Utils::CloneYyjsonValue(value);
    }
    settings_ = std::move(settings);
}

DasResult GraphBoundaryAuthoringSession::GetDocument(
    Das::ExportInterface::IDasJson* /*p_request_json*/,
    Das::ExportInterface::IDasJson** pp_out_document_json)
{
    if (pp_out_document_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_document_json = nullptr;

    // 严格 Get envelope = {document, diagnostics}：document 纯投影现算，
    // diagnostics 恒空数组（本会话无编译期诊断源）。
    const auto ports = LoadBoundaryPorts(settings_);

    yyjson::value envelope(Das::Utils::MakeYyjsonObject());
    auto          env_obj = envelope.as_object();
    (*env_obj)[std::string_view("document")] =
        BuildDocumentValue(role_, ports, revision_);
    (*env_obj)[std::string_view("diagnostics")] = Das::Utils::MakeYyjsonArray();

    auto wrapped = Das::Utils::WrapJson(std::move(envelope));
    *pp_out_document_json = wrapped.Get();
    if (*pp_out_document_json == nullptr)
    {
        return DAS_E_FAIL;
    }
    (*pp_out_document_json)->AddRef();
    return DAS_S_OK;
}

DasResult GraphBoundaryAuthoringSession::ApplyChange(
    Das::ExportInterface::IDasJson*  p_request_json,
    Das::ExportInterface::IDasJson** pp_out_result_json)
{
    if (pp_out_result_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_result_json = nullptr;

    std::vector<Contract::Diagnostic> diagnostics;
    ApplyRequest                      request;
    bool                              request_ok = false;
    if (auto parsed = Das::Utils::ReadJson(p_request_json))
    {
        request_ok = ParseApplyRequest(*parsed, request, diagnostics);
    }
    else
    {
        diagnostics.push_back(
            MakeErrorDiagnostic("Apply request could not be parsed as JSON"));
    }

    bool revision_ok = false;
    if (request_ok)
    {
        if (request.base_revision == revision_)
        {
            revision_ok = true;
        }
        else
        {
            diagnostics.push_back(MakeErrorDiagnostic(
                "baseRevision mismatch: expected " + std::to_string(revision_)
                + " got " + std::to_string(request.base_revision)));
        }
    }

    // 变更分派（词表：item 家族 + addNodePort 家族 + setValue，全部既有
    // kind 字符串，会话解释成口操作）。拒绝零突变：settings 与 revision
    // 保持原样，envelope 携带 error 诊断。
    std::vector<BoundaryPortEntry> candidate = LoadBoundaryPorts(settings_);
    bool                           accepted = false;
    if (request_ok && revision_ok)
    {
        accepted = DispatchBoundaryChange(
            role_,
            request.kind,
            request.payload,
            candidate,
            diagnostics);
    }

    // 先装配信封再提交状态（「拒绝零突变」的返回码语义边角收口）：接受
    // 路径的 settings/revision 提交挪到 WrapJson 成功之后——装配失败
    // （≈OOM，返回 DAS_E_FAIL）时调用方视为 providerFailed 整批拒绝，
    // 会话内存态保持「已拒绝=零突变」（与类注释承诺一致）。next_settings
    // 预写候选（仅接受路径），document 按结果 revision 预投影（成功 =
    // base+1；拒绝 = 原值——拒绝路径的 candidate 从未被改动，直接投影
    // 即未变状态）。
    yyjson::value next_settings = CloneSettings(settings_);
    if (accepted)
    {
        WriteBoundaryPorts(next_settings, candidate);
    }

    // 严格 Apply envelope = {acceptedProperties, document, diagnostics}。
    yyjson::value envelope(Das::Utils::MakeYyjsonObject());
    auto          env_obj = envelope.as_object();
    (*env_obj)[std::string_view("acceptedProperties")] =
        CloneSettings(next_settings);
    (*env_obj)[std::string_view("document")] = BuildDocumentValue(
        role_,
        candidate,
        accepted ? revision_ + 1 : revision_);
    (*env_obj)[std::string_view("diagnostics")] =
        BuildDiagnosticsValue(diagnostics);

    auto wrapped = Das::Utils::WrapJson(std::move(envelope));
    if (wrapped.Get() == nullptr)
    {
        return DAS_E_FAIL;
    }
    if (accepted)
    {
        settings_ = std::move(next_settings);
        ++revision_;
    }
    *pp_out_result_json = wrapped.Get();
    (*pp_out_result_json)->AddRef();
    return DAS_S_OK;
}

DasResult GraphBoundaryAuthoringSession::Compile(
    Das::ExportInterface::IDasJson* /*p_request_json*/,
    Das::ExportInterface::IDasJson** pp_out_result_json)
{
    if (pp_out_result_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_result_json = nullptr;

    // 严格 Compile envelope = {compile, diagnostics}。compile = 从
    // settings 推导的边界签名（与文档 ports section 同构）——供调试/对
    // 账；运行期编译权威仍是 GraphCompiler（图编译经注入的文档口面消费
    // 本签名，不经此出口）。
    const auto ports = LoadBoundaryPorts(settings_);

    yyjson::value compile_value(Das::Utils::MakeYyjsonObject());
    (*compile_value.as_object())[std::string_view("ports")] =
        BuildPortsSection(ports, role_);

    yyjson::value envelope(Das::Utils::MakeYyjsonObject());
    auto          env_obj = envelope.as_object();
    (*env_obj)[std::string_view("compile")] = std::move(compile_value);
    (*env_obj)[std::string_view("diagnostics")] = Das::Utils::MakeYyjsonArray();

    auto wrapped = Das::Utils::WrapJson(std::move(envelope));
    *pp_out_result_json = wrapped.Get();
    if (*pp_out_result_json == nullptr)
    {
        return DAS_E_FAIL;
    }
    (*pp_out_result_json)->AddRef();
    return DAS_S_OK;
}

DAS_CORE_GRAPHRUNTIME_NS_END
