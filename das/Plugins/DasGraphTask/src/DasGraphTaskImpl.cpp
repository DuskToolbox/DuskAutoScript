#include "GraphTaskFocusLog.h"
#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasGuidHolder.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/TaskComponentPorts.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasStringVector.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/header/IDasStringVector.generated.h>

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

        // 执行成功路径的返回码决策：end 节点在场且类型化 outputs 含「值」
        // 整数 → 原样透传（含负值——用户图连负值到 end.值 是「任务失败」
        // 的表达通道，不得夹 0）；end 在场但无「值」→ 0 + WARN 记 end 节
        // 点 id；end 缺席 → 0 + WARN 记运行期实际最后 Done 的节点 id（经
        // 引擎 getter 透出——计划执行序末位在尾部节点被信号门跳过时
        // 并未真执行过，报它是误导）。解析全程容错——任一环节失败回落 0，
        // 不为返回码通道引入新失败面。值源=类型化 outputs PortMap 的「值」
        // 口（D-21⑤，旧 result JSON 的 graphOutputs 段已随单一通道换轨退
        // 役）。
        DasResult ResolveTaskReturnCode(
            const std::string&                         artifact_text,
            const std::string&                         last_executed_node_id,
            Das::ExportInterface::IDasReadOnlyPortMap* typed_outputs,
            std::int64_t*                              p_resolved_end_value)
        {
            std::optional<std::string> end_node_id;
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
                    }
                }
            }
            catch (...)
            {
                // 解析失败回落宽容路径：end 缺席语义（返回 0）。
            }
            if (typed_outputs != nullptr)
            {
                int64_t                 end_value = 0;
                const std::string       value_key_text{END_VALUE_PORT_ID};
                const DasReadOnlyString value_key{value_key_text.c_str()};
                if (DAS::IsOk(
                        typed_outputs->GetInt(value_key.Get(), &end_value)))
                {
                    exported_end_value = end_value;
                }
            }

            if (end_node_id.has_value())
            {
                if (exported_end_value.has_value())
                {
                    if (p_resolved_end_value != nullptr)
                    {
                        *p_resolved_end_value = *exported_end_value;
                    }
                    return static_cast<DasResult>(*exported_end_value);
                }
                DAS_LOG_WARNING(
                    DAS_FMT_NS::format(
                        "Graph end node = {} exported no value port; task "
                        "return code falls back to 0",
                        *end_node_id)
                        .c_str());
                if (p_resolved_end_value != nullptr)
                {
                    *p_resolved_end_value = 0;
                }
                return DAS_S_OK;
            }
            DAS_LOG_WARNING(
                DAS_FMT_NS::format(
                    "Graph has no end node; task return code falls back to 0 "
                    "(last executed node = {})",
                    last_executed_node_id.empty() ? std::string("(none)")
                                                  : last_executed_node_id)
                    .c_str());
            if (p_resolved_end_value != nullptr)
            {
                *p_resolved_end_value = 0;
            }
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

        // Execute compiled artifact via GraphRuntime over the typed plane
        // (D-21 单通道)：graph_inputs=整个 input map（executionInput 键是
        // 协议保留键，广播覆盖层对其跳过、悬空同名口回落声明默认），out
        // puts=类型化边界导出（含不可序列化对象引用）。result 串口的
        // graphOutputs 序列化段已退役——边界值唯一出口=本 PortMap。
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> typed_outputs;
        hr = runtime->ExecuteWithTypedIO(
            p_artifact.Get(),
            stop_token,
            p_input_port_map,
            typed_outputs.Put());

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

        DAS::DasPtr<Das::ExportInterface::IDasPortMap> output_map;
        hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }

        // 类型化输出直写：GetKeys 枚举逐键按运行期类型转写（无硬编码口
        // 名表——边界口集由编译产物决定）。
        if (typed_outputs.Get() != nullptr)
        {
            DAS::DasPtr<Das::ExportInterface::IDasStringVector> output_keys;
            if (DAS::IsOk(typed_outputs->GetKeys(output_keys.Put()))
                && output_keys)
            {
                uint64_t output_key_count = 0;
                if (DAS::IsOk(output_keys->Size(&output_key_count)))
                {
                    for (uint64_t key_index = 0; key_index < output_key_count;
                         ++key_index)
                    {
                        IDasReadOnlyString* key_string = nullptr;
                        if (DAS::IsOk(output_keys->At(key_index, &key_string))
                            && key_string != nullptr)
                        {
                            const char* key_utf8 = nullptr;
                            key_string->GetUtf8(&key_utf8);
                            const std::string output_port_name(
                                key_utf8 != nullptr ? key_utf8 : "");
                            key_string->Release();
                            if (!output_port_name.empty())
                            {
                                Das::Utils::CopyPortMapEntry(
                                    typed_outputs.Get(),
                                    output_port_name,
                                    output_map.Get(),
                                    output_port_name);
                            }
                        }
                    }
                }
            }
        }

        *pp_out_port_map = output_map.Get();
        if (output_map.Get())
        {
            output_map.Get()->AddRef();
        }
        // 无 end 图的 WARN 口径=运行期实际最后 Done 的节点（经引擎 getter
        // 透出）；getter 失败回落空串（WARN 显 "(none)"），不为返回码通
        // 道引入新失败面。
        std::string last_executed_node_id;
        {
            DAS::DasPtr<IDasReadOnlyString> p_last_node;
            if (DAS::IsOk(runtime->GetLastExecutedNodeId(p_last_node.Put()))
                && p_last_node.Get() != nullptr)
            {
                const char* node_utf8 = nullptr;
                if (DAS::IsOk(p_last_node->GetUtf8(&node_utf8))
                    && node_utf8 != nullptr)
                {
                    last_executed_node_id = node_utf8;
                }
            }
        }
        // 返回码链：end.值 经类型化 outputs 的「值」口读取（D-21⑤）。
        std::int64_t    resolved_end_value = 0;
        const DasResult task_result = ResolveTaskReturnCode(
            artifact_text,
            last_executed_node_id,
            typed_outputs.Get(),
            &resolved_end_value);
        // 完成事件落 focus 通道（user.jsonl → UI 事件日志）：end 值经插件
        // i18n 模板渲染；发布失败只落调试日志，不影响任务返回码。
        PublishGraphTaskFinishedFocusEntry(resolved_end_value);
        return task_result;
    }

} // namespace Das::Plugins::DasGraphTask
