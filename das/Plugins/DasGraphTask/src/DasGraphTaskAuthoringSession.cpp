#define DAS_BUILD_SHARED

#include "DasGraphTaskAuthoringSession.h"

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasSchedulerService.h>

#include <mutex>
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

        // 主进程调度服务的具名注册名（与 DasFlowControl 调用组件同注册名）。
        constexpr std::string_view SCHEDULER_SERVICE_NAME = "das.scheduler";

        // 内部条目文档获取请求：携带投影关闭旗标——被获取条目的 Provider
        // 会话收到同一请求后不再做边界投影，引用环（A 引 B、B 引 A）的
        // Get 投影递归在此断开。
        constexpr std::string_view ENTRY_DOCUMENT_REQUEST =
            R"({"boundaryPortProjection":false})";

        // 条目文档获取缝（测试可注入；空 = 生产实现）。互斥保护：注入与
        // Get 可能来自不同线程。
        std::mutex& FetcherMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        RepositoryEntryDocumentFetcher& FetcherSlot()
        {
            static RepositoryEntryDocumentFetcher fetcher;
            return fetcher;
        }

        // 条目文档获取的生产实现：经主进程 C ABI 查 IDasSchedulerService
        // 的条目 authoring Get（跨进程服务访问必须走 C ABI——插件内联读
        // IPC 上下文的 thread_local 跨 DLL 分裂恒空）。
        std::optional<std::string> FetchEntryDocumentViaScheduler(
            int64_t entry_id)
        {
            DasPtr<IDasBase> service_base;
            if (DAS::IsFailed(DasQueryMainProcessInterfaceByName(
                    std::string(SCHEDULER_SERVICE_NAME).c_str(),
                    service_base.Put()))
                || !service_base)
            {
                return std::nullopt;
            }

            DasPtr<IDasSchedulerService> scheduler;
            if (DAS::IsFailed(service_base.As(scheduler.Put())) || !scheduler)
            {
                return std::nullopt;
            }

            auto request =
                MakeReadOnlyString(std::string(ENTRY_DOCUMENT_REQUEST));
            DasPtr<IDasReadOnlyString> response;
            if (DAS::IsFailed(scheduler->GetRepositoryEntryAuthoringDocument(
                    entry_id,
                    request.Get(),
                    response.Put()))
                || !response)
            {
                return std::nullopt;
            }
            const char* response_utf8 = nullptr;
            if (DAS::IsFailed(response->GetUtf8(&response_utf8))
                || response_utf8 == nullptr)
            {
                return std::nullopt;
            }
            return std::string(response_utf8);
        }

        RepositoryEntryDocumentFetcher ActiveEntryDocumentFetcher()
        {
            const std::scoped_lock lock(FetcherMutex());
            if (FetcherSlot())
            {
                return FetcherSlot();
            }
            return &FetchEntryDocumentViaScheduler;
        }

        // 从 Get 响应提取条目持久化源文档（acceptedProperties，
        // GraphDocumentDto 形态）——边界推导的输入。
        std::optional<std::string> ExtractAcceptedPropertiesText(
            const std::string& response_text)
        {
            try
            {
                auto response = yyjson::read(response_text);
                auto response_object = response.as_object();
                if (!response_object.has_value()
                    || !response_object->contains(
                        std::string_view("acceptedProperties")))
                {
                    return std::nullopt;
                }
                auto serialized = Das::Utils::SerializeYyjsonValue(
                    (*response_object)[std::string_view("acceptedProperties")]);
                if (!serialized.has_value())
                {
                    return std::nullopt;
                }
                return *serialized;
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

        // GetDocument 后处理主体：graph 视图内每个已绑 graph 条目的调用节
        // 点注入条目边界端口。任一步失败降级零注入（投影尽力而为，不阻断
        // Get）。返回空 = 无修改（原样透传）。
        std::optional<std::string> InjectBoundaryPortsIntoDocument(
            GraphAuthoringSessionState* state,
            const std::string&          envelope_text)
        {
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
            const auto fetcher = ActiveEntryDocumentFetcher();
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
                    auto response_text = fetcher(*entry_id);
                    if (!response_text.has_value())
                    {
                        continue;
                    }
                    auto document_source =
                        ExtractAcceptedPropertiesText(*response_text);
                    if (!document_source.has_value())
                    {
                        continue;
                    }
                    DasPtr<IDasReadOnlyString> boundary_text;
                    auto document_str = MakeReadOnlyString(*document_source);
                    if (DAS::IsFailed(GraphDeriveBoundaryPorts(
                            state,
                            document_str.Get(),
                            boundary_text.Put()))
                        || !boundary_text)
                    {
                        continue;
                    }
                    const char* boundary_utf8 = nullptr;
                    if (DAS::IsFailed(boundary_text->GetUtf8(&boundary_utf8))
                        || boundary_utf8 == nullptr)
                    {
                        continue;
                    }
                    try
                    {
                        auto boundary =
                            yyjson::read(std::string(boundary_utf8));
                        cached = boundary_cache
                                     .emplace(*entry_id, std::move(boundary))
                                     .first;
                    }
                    catch (...)
                    {
                        continue;
                    }
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

    void SetRepositoryEntryDocumentFetcherForTest(
        RepositoryEntryDocumentFetcher fetcher)
    {
        const std::scoped_lock lock(FetcherMutex());
        FetcherSlot() = std::move(fetcher);
    }

    DasGraphTaskAuthoringSession::DasGraphTaskAuthoringSession(
        std::string                             context_json,
        PluginInterface::IDasTaskComponentHost* p_creator)
    {
        auto ctx_str = MakeReadOnlyString(context_json);
        // CreateGraphAuthoringSession seeds the Core-owned GraphDocumentDto
        // store from the context (formSequence / graph / empty) —含
        // actionCatalog/faces 键的自解析（SeedFromContext）。插件是纯管
        // 道：完整 context 原文透传，零提取/零转发跳。creator 借用指针
        // 随会话下传（D-07：与执行侧 CreateGraphRuntimeWithHost 对称，
        // 本尊生命周期由 PluginPackage 的 host_ 保证）。
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
                state_,
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
