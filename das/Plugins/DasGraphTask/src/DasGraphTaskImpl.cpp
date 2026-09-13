#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasGuidHolder.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>

#include <optional>
#include <string>
#include <string_view>

namespace Das::Plugins::DasGraphTask
{
    namespace
    {
        // end 组件的静态「值」口名（manifest end definition 的 port id 镜
        // 像——返回码的唯一数据源口）。
        constexpr std::string_view END_VALUE_PORT_ID = "值";

        // 执行成功路径的返回码决策：end 节点在场且 graphOutputs 含「值」
        // 整数 → 原样透传（含负值——用户图连负值到 end.值 是「任务失败」
        // 的表达通道，不得夹 0）；end 在场但无「值」→ 0 + WARN 记 end 节
        // 点 id；end 缺席 → 0 + WARN 记 execution_order 末节点 id（宽容语
        // 义）。解析全程容错——任一环节失败回落 0，不为返回码通道引入新
        // 失败面。
        DasResult ResolveTaskReturnCode(
            const std::string& artifact_text,
            const std::string& result_text)
        {
            std::optional<std::string> end_node_id;
            std::optional<std::string> last_executed_node_id;
            std::optional<int64_t>     exported_end_value;
            try
            {
                if (!artifact_text.empty())
                {
                    auto shell = yyjson::read(artifact_text);
                    auto shell_obj = shell.as_object();
                    if (shell_obj
                        && shell_obj->contains(std::string_view("plan")))
                    {
                        const auto& plan_subtree =
                            (*shell_obj)[std::string_view("plan")];
                        auto plan_obj = plan_subtree.as_object();
                        if (plan_obj.has_value()
                            && plan_obj->contains(
                                std::string_view("nodeSnapshots")))
                        {
                            auto snapshots =
                                (*plan_obj)[std::string_view("nodeSnapshots")]
                                    .as_array();
                            if (snapshots.has_value())
                            {
                                for (const auto& snapshot : *snapshots)
                                {
                                    auto snapshot_obj = snapshot.as_object();
                                    if (!snapshot_obj.has_value())
                                    {
                                        continue;
                                    }
                                    const auto component_guid =
                                        (*snapshot_obj)[std::string_view(
                                                            "componentGuid")]
                                            .as_string();
                                    if (component_guid.has_value()
                                        && *component_guid
                                               == Das::Core::GraphRuntime::
                                                   GRAPH_END_COMPONENT_GUID)
                                    {
                                        end_node_id =
                                            (*snapshot_obj)[std::string_view(
                                                                "nodeId")]
                                                .as_string();
                                    }
                                }
                            }
                        }
                        if (plan_obj.has_value()
                            && plan_obj->contains(
                                std::string_view("executionOrder")))
                        {
                            auto execution_order =
                                (*plan_obj)[std::string_view("executionOrder")]
                                    .as_array();
                            if (execution_order.has_value()
                                && !execution_order->empty())
                            {
                                last_executed_node_id =
                                    execution_order->back().as_string();
                            }
                        }
                    }
                }
            }
            catch (...)
            {
                // 解析失败回落宽容路径：end 缺席语义（返回 0）。
            }
            try
            {
                if (!result_text.empty())
                {
                    auto result_root = yyjson::read(result_text);
                    auto result_obj = result_root.as_object();
                    if (result_obj.has_value()
                        && result_obj->contains(
                            std::string_view("graphOutputs")))
                    {
                        auto graph_outputs =
                            (*result_obj)[std::string_view("graphOutputs")]
                                .as_object();
                        if (graph_outputs.has_value()
                            && graph_outputs->contains(
                                std::string_view(END_VALUE_PORT_ID)))
                        {
                            exported_end_value =
                                (*graph_outputs)[std::string_view(
                                                     END_VALUE_PORT_ID)]
                                    .as_int();
                        }
                    }
                }
            }
            catch (...)
            {
                // graphOutputs 解析失败按「无值」处理。
            }

            if (end_node_id.has_value())
            {
                if (exported_end_value.has_value())
                {
                    return static_cast<DasResult>(*exported_end_value);
                }
                DAS_LOG_WARNING(
                    DAS_FMT_NS::format(
                        "Graph end node = {} exported no value port; task "
                        "return code falls back to 0",
                        *end_node_id)
                        .c_str());
                return DAS_S_OK;
            }
            DAS_LOG_WARNING(
                DAS_FMT_NS::format(
                    "Graph has no end node; task return code falls back to 0 "
                    "(last executed node = {})",
                    last_executed_node_id.value_or(std::string("(none)")))
                    .c_str());
            return DAS_S_OK;
        }
    } // namespace

    DasGraphTaskImpl::DasGraphTaskImpl(
        Das::PluginInterface::IDasTaskComponentHost* p_host)
        : host_(p_host)
    {
    }

    DasResult DasGraphTaskImpl::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<DasGraphTaskImpl>();
        return DAS_S_OK;
    }

    DasResult DasGraphTaskImpl::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.Plugins.DasGraphTask.DasGraphTaskImpl",
            pp_out_name);
    }

    DasResult DasGraphTaskImpl::ApplySettingsChange(
        Das::ExportInterface::IDasJson*  p_request_json,
        Das::ExportInterface::IDasJson** pp_out_result_json)
    {
        std::ignore = p_request_json;
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;
        return DAS_S_OK;
    }

    DasResult DasGraphTaskImpl::Do(
        Das::PluginInterface::IDasStopToken*       stop_token,
        Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        Das::ExportInterface::IDasPortMap**        pp_out_port_map)
    {
        if (pp_out_port_map == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_port_map = nullptr;

        if (!host_)
        {
            last_error_ = "Task component host is unavailable";
            DAS_LOG_ERROR(last_error_.c_str());
            return DAS_E_INVALID_POINTER;
        }

        // Read compiled artifact string from "executionInput" port
        DAS::DasPtr<IDasReadOnlyString> p_artifact;
        DasResult                       hr = DAS_S_OK;
        std::string                     artifact_text;
        if (p_input_port_map != nullptr)
        {
            const DasReadOnlyString execution_input_key{"executionInput"};
            hr = p_input_port_map->GetString(
                execution_input_key.Get(),
                p_artifact.Put());
            // It's OK if the port doesn't exist — we pass null artifact.
            if (DAS::IsOk(hr) && p_artifact.Get() != nullptr)
            {
                const char* artifact_utf8 = nullptr;
                if (DAS::IsOk(p_artifact->GetUtf8(&artifact_utf8))
                    && artifact_utf8 != nullptr)
                {
                    artifact_text = artifact_utf8;
                }
            }
        }

        // Create GraphRuntime with host bound at construction.
        DAS::DasPtr<Das::ExportInterface::IDasGraphRuntime> runtime;
        hr = CreateGraphRuntimeWithHost(host_.Get(), runtime.Put());
        if (DAS::IsFailed(hr))
        {
            last_error_ = "Failed to create GraphRuntime";
            DAS_LOG_ERROR(last_error_.c_str());
            return hr;
        }

        // Execute compiled artifact via GraphRuntime
        DAS::DasPtr<Das::ExportInterface::IDasJson> result_json;
        hr = runtime->Execute(p_artifact.Get(), stop_token, result_json.Put());

        if (DAS::IsFailed(hr))
        {
            DAS::DasPtr<IDasReadOnlyString> p_error;
            if (DAS::IsOk(runtime->GetErrorMessage(p_error.Put())) && p_error)
            {
                const char* utf8 = nullptr;
                p_error->GetUtf8(&utf8);
                if (utf8 && *utf8)
                {
                    last_error_ = utf8;
                }
            }
            if (last_error_.empty())
            {
                last_error_ = "Graph execution failed";
            }
            DAS_LOG_ERROR(last_error_.c_str());
            return hr;
        }

        // Serialize result JSON → string, put into output PortMap under
        // "result" port.
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> output_map;
        hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }

        std::string result_text;
        if (result_json.Get())
        {
            DAS::DasPtr<IDasReadOnlyString> p_result_str;
            hr = result_json->ToString(0, p_result_str.Put());
            if (DAS::IsOk(hr) && p_result_str.Get())
            {
                const char* result_utf8 = nullptr;
                if (DAS::IsOk(p_result_str->GetUtf8(&result_utf8))
                    && result_utf8 != nullptr)
                {
                    result_text = result_utf8;
                }
                const DasReadOnlyString result_key{"result"};
                output_map->SetString(result_key.Get(), p_result_str.Get());
            }
        }

        *pp_out_port_map = output_map.Get();
        if (output_map.Get())
        {
            output_map.Get()->AddRef();
        }
        // 返回码链：end.值 经 boundary export 进 result 的 graphOutputs
        // 段，成功路径按其存在性分支返回（result 串口写入保持兼容通道）。
        return ResolveTaskReturnCode(artifact_text, result_text);
    }

} // namespace Das::Plugins::DasGraphTask
