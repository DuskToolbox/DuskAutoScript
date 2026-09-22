#define DAS_BUILD_SHARED

#include "DasGraphTaskAuthoringSession.h"

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    namespace
    {
        // -------------------------------------------------------------------
        // IDasJson <-> UTF-8 string marshaling (the Core C ABI speaks JSON
        // strings; the authoring-session COM boundary speaks IDasJson).
        // -------------------------------------------------------------------

        std::string JsonToUtf8(ExportInterface::IDasJson* json)
        {
            if (json == nullptr)
            {
                return {};
            }
            DasPtr<IDasReadOnlyString> text;
            if (DAS::IsFailed(json->ToString(0, text.Put()))
                || text.Get() == nullptr)
            {
                return {};
            }
            const char* utf8 = nullptr;
            if (DAS::IsFailed(text->GetUtf8(&utf8)) || utf8 == nullptr)
            {
                return {};
            }
            return std::string(utf8);
        }

        DasPtr<IDasReadOnlyString> MakeReadOnlyString(const std::string& s)
        {
            DasPtr<IDasReadOnlyString> ro;
            CreateIDasReadOnlyStringFromUtf8(s.c_str(), ro.Put());
            return ro;
        }

        // 内部条目文档获取请求：携带投影关闭旗标——被获取条目的 Provider
        // 会话收到同一请求后不再做边界投影，引用环（A 引 B、B 引 A）的
        // Get 投影递归在此断开。
        constexpr std::string_view ENTRY_DOCUMENT_REQUEST =
            R"({"boundaryPortProjection":false})";

        // creator 通道版条目文档获取：开被引条目自己的 authoring 会话再
        // Get 文档（请求体携带投影关闭旗标）。creator 借用指针来自插件包
        // 加载期注入（经代理可达主进程本尊——插件禁内联读 IPC 上下文）。
        // 任务类型 guid 取节点组件 guid（绑条目节点的组件 guid 与条目任务
        // 类型 guid 同值域——插件侧无注册表面可反查，与 Core 侧无 manager
        // 回退同一判据）；context 的 repositoryEntryId 键由解析后端预载条
        // 目持久化 properties。任何失败返回 nullopt（调用方走既有缺文档
        // 路径降级零注入——投影期非编译期，不引入新错误面）。
        std::optional<std::string> FetchEntryDocumentViaCreator(
            PluginInterface::IDasTaskComponentCreator* creator,
            const std::string&                         component_guid_text,
            int64_t                                    entry_id)
        {
            if (creator == nullptr || component_guid_text.empty())
            {
                return std::nullopt;
            }
            DasGuid task_type_guid{};
            if (DAS::IsFailed(DasMakeDasGuid(
                    component_guid_text.c_str(),
                    &task_type_guid)))
            {
                return std::nullopt;
            }

            yyjson::value context(Das::Utils::MakeYyjsonObject());
            {
                auto context_object = context.as_object();
                (*context_object)[std::string_view("repositoryEntryId")] =
                    entry_id;
            }
            auto context_text =
                Das::Utils::SerializeYyjsonValue(std::move(context));
            if (!context_text.has_value())
            {
                return std::nullopt;
            }
            DasPtr<ExportInterface::IDasJson> context_json;
            if (DAS::IsFailed(ParseDasJsonFromString(
                    context_text->c_str(),
                    context_json.Put()))
                || !context_json)
            {
                return std::nullopt;
            }

            DasPtr<PluginInterface::IDasTaskAuthoringSession> session;
            if (DAS::IsFailed(creator->CreateAuthoringSession(
                    task_type_guid,
                    context_json.Get(),
                    session.Put()))
                || !session)
            {
                return std::nullopt;
            }

            DasPtr<ExportInterface::IDasJson> request_json;
            if (DAS::IsFailed(ParseDasJsonFromString(
                    std::string(ENTRY_DOCUMENT_REQUEST).c_str(),
                    request_json.Put()))
                || !request_json)
            {
                return std::nullopt;
            }
            DasPtr<ExportInterface::IDasJson> document_json;
            if (DAS::IsFailed(session->GetDocument(
                    request_json.Get(),
                    document_json.Put()))
                || !document_json)
            {
                return std::nullopt;
            }
            return JsonToUtf8(document_json.Get());
        }

        // 会话 Get 响应 → 边界合并形状：解信封 document 键，读文档根
        // ports 自述（条目边界口面——与编译期注入 pass 消费同一文档面），
        // flat {id,label?,type,direction} 条目转
        // {inputs:[{portId,displayLabel,portType}],outputs:[...]}。无根
        // ports 自述返回 nullopt（缺文档路径：调用方降级零注入）。
        std::optional<yyjson::value> ExtractBoundaryPortsFromSessionDocument(
            const std::string& response_text)
        {
            try
            {
                yyjson::value response = yyjson::read(response_text);
                auto          response_object = response.as_object();
                if (!response_object.has_value())
                {
                    return std::nullopt;
                }
                yyjson::value* document = &response;
                yyjson::value  unwrapped;
                if (response_object->contains(std::string_view("document")))
                {
                    unwrapped = Das::Utils::CloneYyjsonValue(
                        (*response_object)[std::string_view("document")]);
                    document = &unwrapped;
                }
                auto document_object = document->as_object();
                if (!document_object.has_value()
                    || !document_object->contains(std::string_view("ports")))
                {
                    return std::nullopt;
                }
                auto ports =
                    (*document_object)[std::string_view("ports")].as_array();
                if (!ports.has_value())
                {
                    return std::nullopt;
                }

                auto boundary = Das::Utils::MakeYyjsonObject();
                auto inputs = Das::Utils::MakeYyjsonArray();
                auto outputs = Das::Utils::MakeYyjsonArray();
                auto inputs_array = inputs.as_array();
                auto outputs_array = outputs.as_array();
                for (const auto& port : *ports)
                {
                    auto port_object = port.as_object();
                    if (!port_object.has_value())
                    {
                        continue;
                    }
                    auto port_id =
                        (*port_object)[std::string_view("id")].as_string();
                    if (!port_id.has_value()
                        && port_object->contains(std::string_view("portId")))
                    {
                        port_id = (*port_object)[std::string_view("portId")]
                                      .as_string();
                    }
                    if (!port_id.has_value() || port_id->empty())
                    {
                        continue;
                    }
                    auto port_type =
                        (*port_object)[std::string_view("type")].as_string();
                    if (!port_type.has_value()
                        && port_object->contains(std::string_view("portType")))
                    {
                        port_type = (*port_object)[std::string_view("portType")]
                                        .as_string();
                    }
                    auto direction =
                        (*port_object)[std::string_view("direction")]
                            .as_string();
                    if (!direction.has_value())
                    {
                        continue;
                    }
                    auto entry = Das::Utils::MakeYyjsonObject();
                    auto entry_object = entry.as_object();
                    (*entry_object)[std::string_view("portId")] =
                        std::make_pair(
                            std::string_view(*port_id),
                            yyjson::copy_string);
                    // displayLabel 键恒在场（消费方 object[] 对缺失键抛
                    // 异常）；label 缺席时回落 port_id（与编译侧根 ports
                    // 解析的回落一致）。
                    std::string_view display_label{*port_id};
                    if (port_object->contains(std::string_view("label")))
                    {
                        auto label = (*port_object)[std::string_view("label")]
                                         .as_string();
                        if (label.has_value() && !label->empty())
                        {
                            display_label = *label;
                        }
                    }
                    (*entry_object)[std::string_view("displayLabel")] =
                        std::make_pair(display_label, yyjson::copy_string);
                    (*entry_object)[std::string_view("portType")] =
                        std::make_pair(
                            std::string_view(
                                port_type.has_value()
                                    ? std::string_view(*port_type)
                                    : std::string_view("string")),
                            yyjson::copy_string);
                    if (*direction == std::string_view("input"))
                    {
                        inputs_array->emplace_back(std::move(entry));
                    }
                    else if (*direction == std::string_view("output"))
                    {
                        outputs_array->emplace_back(std::move(entry));
                    }
                }
                auto boundary_object = boundary.as_object();
                (*boundary_object)[std::string_view("inputs")] =
                    Das::Utils::CloneYyjsonValue(inputs);
                (*boundary_object)[std::string_view("outputs")] =
                    Das::Utils::CloneYyjsonValue(outputs);
                return boundary;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        // 请求旗标：内部条目获取请求显式关闭边界投影（递归防护）。仅识别
        // {"boundaryPortProjection": false} 精确形态。
        bool RequestDisablesBoundaryProjection(const std::string& request_text)
        {
            if (request_text.empty())
            {
                return false;
            }
            try
            {
                auto request = yyjson::read(request_text);
                auto request_object = request.as_object();
                if (!request_object.has_value()
                    || !request_object->contains(
                        std::string_view("boundaryPortProjection")))
                {
                    return false;
                }
                auto flag = (*request_object)[std::string_view(
                                                  "boundaryPortProjection")]
                                .as_bool();
                return flag.has_value() && !*flag;
            }
            catch (...)
            {
                return false;
            }
        }

        // 构造一个 GraphNodePort 严格 wire 形（五键，category 由 port_type
        // 派生——signal 即执行流口，其余为数据口，与 Core 投影同词汇）。
        // 模板形参承接 const_value_ref（yyjson 值引用家族与 owned 值不同
        // 型，非模板签名无法同时收两边）。
        template <typename BoundaryPortValue>
        yyjson::value MakeProjectedPort(
            const BoundaryPortValue& boundary_port,
            std::string_view         direction)
        {
            auto port_object_value = Das::Utils::MakeYyjsonObject();
            auto port_object = port_object_value.as_object();
            auto boundary_port_object = boundary_port.as_object();
            if (!boundary_port_object.has_value())
            {
                return port_object_value;
            }
            auto port_id = (*boundary_port_object)[std::string_view("portId")]
                               .as_string()
                               .value_or("");
            auto display_label =
                (*boundary_port_object)[std::string_view("displayLabel")]
                    .as_string()
                    .value_or("");
            auto port_type =
                (*boundary_port_object)[std::string_view("portType")]
                    .as_string()
                    .value_or("");
            (*port_object)[std::string_view("portId")] =
                std::make_pair(std::string_view(port_id), yyjson::copy_string);
            (*port_object)[std::string_view("displayLabel")] = std::make_pair(
                std::string_view(
                    display_label.empty() ? port_id : display_label),
                yyjson::copy_string);
            (*port_object)[std::string_view("portType")] = std::make_pair(
                std::string_view(port_type),
                yyjson::copy_string);
            (*port_object)[std::string_view("direction")] =
                std::make_pair(direction, yyjson::copy_string);
            (*port_object)[std::string_view("category")] = std::make_pair(
                port_type == std::string_view("signal")
                    ? std::string_view("execution")
                    : std::string_view("data"),
                yyjson::copy_string);
            return port_object_value;
        }

        // 调用节点边界端口投影：把条目边界端口幂等合入该节点投影的
        // ports[]（每次 Get 现算现投影，不落盘——条目内编辑下次 Get 即反
        // 映，无跨文档持久传播）。返回 false = 无可注入（调用方原样透传）。
        // NodeValue 模板形参承接数组元素视图（可变改写经视图传播）。
        template <typename NodeValue>
        bool MergeBoundaryPortsIntoNode(
            NodeValue&&          node_value,
            const yyjson::value& boundary)
        {
            auto node_object = node_value.as_object();
            if (!node_object.has_value())
            {
                return false;
            }

            // 既有 (portId, direction) 键集：幂等合入依据。
            std::unordered_set<std::string> existing_port_keys;
            auto merged_ports = Das::Utils::MakeYyjsonArray();
            auto merged_array = merged_ports.as_array();
            if (node_object->contains(std::string_view("ports")))
            {
                auto ports_array =
                    (*node_object)[std::string_view("ports")].as_array();
                if (ports_array.has_value())
                {
                    for (const auto& port : *ports_array)
                    {
                        auto port_object = port.as_object();
                        if (port_object.has_value())
                        {
                            auto port_id =
                                (*port_object)[std::string_view("portId")]
                                    .as_string()
                                    .value_or("");
                            auto direction =
                                (*port_object)[std::string_view("direction")]
                                    .as_string()
                                    .value_or("");
                            existing_port_keys.insert(
                                std::string(port_id) + ":"
                                + std::string(direction));
                        }
                        merged_array->emplace_back(
                            Das::Utils::CloneYyjsonValue(port));
                    }
                }
            }

            auto boundary_object = boundary.as_object();
            if (!boundary_object.has_value())
            {
                return false;
            }
            bool merged_any = false;
            for (const std::string_view direction_slot :
                 {std::string_view("inputs"), std::string_view("outputs")})
            {
                auto boundary_list =
                    (*boundary_object)[direction_slot].as_array();
                if (!boundary_list.has_value())
                {
                    continue;
                }
                const std::string_view direction =
                    direction_slot == std::string_view("inputs")
                        ? std::string_view("input")
                        : std::string_view("output");
                for (const auto& boundary_port : *boundary_list)
                {
                    auto boundary_port_object = boundary_port.as_object();
                    if (!boundary_port_object.has_value())
                    {
                        continue;
                    }
                    auto port_id =
                        (*boundary_port_object)[std::string_view("portId")]
                            .as_string()
                            .value_or("");
                    if (port_id.empty())
                    {
                        continue;
                    }
                    if (existing_port_keys.contains(
                            std::string(port_id) + ":"
                            + std::string(direction)))
                    {
                        continue;
                    }
                    merged_array->emplace_back(
                        MakeProjectedPort(boundary_port, direction));
                    merged_any = true;
                }
            }
            if (!merged_any)
            {
                return false;
            }
            (*node_object)[std::string_view("ports")] =
                Das::Utils::CloneYyjsonValue(merged_ports);
            return true;
        }

        // 节点已绑条目判定：投影 fields[] 中 type==taskRepositoryRef 且
        // value 为正整数的字段值即条目 id。
        template <typename NodeValue>
        std::optional<int64_t> BoundRepositoryEntryOf(
            const NodeValue& node_value)
        {
            auto node_object = node_value.as_object();
            if (!node_object.has_value()
                || !node_object->contains(std::string_view("fields")))
            {
                return std::nullopt;
            }
            auto fields = (*node_object)[std::string_view("fields")].as_array();
            if (!fields.has_value())
            {
                return std::nullopt;
            }
            for (const auto& field : *fields)
            {
                auto field_object = field.as_object();
                if (!field_object.has_value())
                {
                    continue;
                }
                auto type_word =
                    (*field_object)[std::string_view("type")].as_string();
                if (!type_word.has_value()
                    || *type_word != std::string_view("taskRepositoryRef"))
                {
                    continue;
                }
                auto entry_id =
                    (*field_object)[std::string_view("value")].as_int();
                if (entry_id.has_value() && *entry_id > 0)
                {
                    return *entry_id;
                }
            }
            return std::nullopt;
        }

        // 节点组件 guid 读取（creator 通道取条目会话的任务类型 guid 来
        // 源——绑条目节点的组件 guid 与条目任务类型 guid 同值域）。
        template <typename NodeValue>
        std::string ComponentGuidOfNode(const NodeValue& node_value)
        {
            auto node_object = node_value.as_object();
            if (!node_object.has_value())
            {
                return {};
            }
            return std::string((*node_object)[std::string_view("componentGuid")]
                                   .as_string()
                                   .value_or(""));
        }

        // GetDocument 后处理主体：graph 视图内每个已绑 graph 条目的调用节
        // 点注入条目边界端口。任一步失败降级零注入（投影尽力而为，不阻断
        // Get）。返回空 = 无修改（原样透传）。
        std::optional<std::string> InjectBoundaryPortsIntoDocument(
            PluginInterface::IDasTaskComponentCreator* creator,
            const std::string&                         envelope_text)
        {
            if (creator == nullptr)
            {
                return std::nullopt;
            }
            // 显式持有 owned 可变文档：yyjson::read 的 auto 推导得到的是
            // const 视图 reader，后续节点 ports 的就地改写需要可变链。
            yyjson::value root = yyjson::read(envelope_text);
            auto          root_object = root.as_object();
            if (!root_object.has_value()
                || !root_object->contains(std::string_view("document")))
            {
                return std::nullopt;
            }
            auto&& document_value =
                (*root_object)[std::string_view("document")];
            auto document_object = document_value.as_object();
            if (!document_object.has_value())
            {
                return std::nullopt;
            }
            auto view_kind = (*document_object)[std::string_view("viewKind")]
                                 .as_string()
                                 .value_or("");
            if (view_kind != std::string_view("graph"))
            {
                return std::nullopt;
            }
            // 可变改写链必须逐层经 object 槽位取（对命名视图直接 as_array
            // 是拷贝语义，元素改写不回传文档）。
            auto graph_value = (*document_object)[std::string_view("graph")];
            auto graph_object = graph_value.as_object();
            if (!graph_object.has_value())
            {
                return std::nullopt;
            }
            auto nodes = (*graph_object)[std::string_view("nodes")].as_array();
            if (!nodes.has_value() || nodes->empty())
            {
                return std::nullopt;
            }

            // 逐节点解析绑定；条目文档同 id 只取一次。
            std::unordered_map<int64_t, yyjson::value> boundary_cache;
            bool                                       modified = false;
            for (auto&& node_value : *nodes)
            {
                auto entry_id = BoundRepositoryEntryOf(node_value);
                if (!entry_id.has_value())
                {
                    continue;
                }
                auto cached = boundary_cache.find(*entry_id);
                if (cached == boundary_cache.end())
                {
                    auto response_text = FetchEntryDocumentViaCreator(
                        creator,
                        ComponentGuidOfNode(node_value),
                        *entry_id);
                    if (!response_text.has_value())
                    {
                        continue;
                    }
                    auto boundary =
                        ExtractBoundaryPortsFromSessionDocument(*response_text);
                    if (!boundary.has_value())
                    {
                        continue;
                    }
                    cached =
                        boundary_cache.emplace(*entry_id, std::move(*boundary))
                            .first;
                }
                if (MergeBoundaryPortsIntoNode(node_value, cached->second))
                {
                    modified = true;
                }
            }
            if (!modified)
            {
                return std::nullopt;
            }

            auto serialized = Das::Utils::SerializeYyjsonValue(root);
            if (!serialized.has_value())
            {
                return std::nullopt;
            }
            return *serialized;
        }
    } // namespace

    DasGraphTaskAuthoringSession::DasGraphTaskAuthoringSession(
        std::string                                context_json,
        PluginInterface::IDasTaskComponentCreator* p_creator)
        : creator_(p_creator)
    {
        auto ctx_str = MakeReadOnlyString(context_json);
        // CreateGraphAuthoringSession seeds the Core-owned GraphDocumentDto
        // store from the context (formSequence / graph / empty) —含
        // actionCatalog/faces 键的自解析（SeedFromContext）。插件是纯管
        // 道：完整 context 原文透传，零提取/零转发跳。creator 借用指针
        // 随会话下传（与执行侧 CreateGraphRuntimeWithHost 对称，本尊生
        // 命周期由 PluginPackage 的注入方保证）；同时落到会话成员供
        // GetDocument 投影期取被引条目文档（开条目自己的 authoring 会话）。
        CreateGraphAuthoringSession(p_creator, ctx_str.Get(), &state_);
    }

    DasGraphTaskAuthoringSession::~DasGraphTaskAuthoringSession()
    {
        DestroyGraphAuthoringSession(state_);
        state_ = nullptr;
    }

    DasResult DasGraphTaskAuthoringSession::GetDocument(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_document_json)
    {
        if (pp_out_document_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_document_json = nullptr;

        const std::string request_text = JsonToUtf8(p_request_json);
        auto              req_str = MakeReadOnlyString(request_text);
        DasPtr<Das::ExportInterface::IDasJson> document_json;
        const DasResult hr = GraphAuthoringSessionGetDocument(
            state_,
            req_str.Get(),
            document_json.Put());
        if (DAS::IsFailed(hr) || document_json.Get() == nullptr)
        {
            return hr;
        }

        // 调用节点端口投影（条目边界端口现算现注入）。内部条目获取请求携
        // 带投影关闭旗标（递归防护）；投影失败降级原样透传，不阻断 Get。
        if (!RequestDisablesBoundaryProjection(request_text))
        {
            auto enhanced = InjectBoundaryPortsIntoDocument(
                creator_,
                JsonToUtf8(document_json.Get()));
            if (enhanced.has_value())
            {
                DasPtr<Das::ExportInterface::IDasJson> enhanced_json;
                if (DAS::IsOk(ParseDasJsonFromString(
                        enhanced->c_str(),
                        enhanced_json.Put()))
                    && enhanced_json.Get() != nullptr)
                {
                    *pp_out_document_json = enhanced_json.Get();
                    enhanced_json.Get()->AddRef();
                    return DAS_S_OK;
                }
            }
        }

        *pp_out_document_json = document_json.Get();
        document_json.Get()->AddRef();
        return hr;
    }

    DasResult DasGraphTaskAuthoringSession::ApplyChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        auto change_str = MakeReadOnlyString(JsonToUtf8(p_request_json));
        return GraphAuthoringSessionApplyChange(
            state_,
            change_str.Get(),
            pp_out_result_json);
    }

    DasResult DasGraphTaskAuthoringSession::Compile(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        auto req_str = MakeReadOnlyString(JsonToUtf8(p_request_json));
        return GraphAuthoringSessionCompile(
            state_,
            req_str.Get(),
            pp_out_result_json);
    }
} // namespace Plugins::DasGraphTask
DAS_NS_END
