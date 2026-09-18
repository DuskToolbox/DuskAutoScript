#include "GraphTaskFocusLogComponent.h"

#include "GraphTaskFocusLog.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/Utils/TaskComponentPorts.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>

#include <string>

namespace Das::Plugins::DasGraphTask
{
    namespace
    {
        // char 数组而非 string_view：DasReadOnlyString 构造只收 const char*
        // （须终止符），GetPortString 收 string_view——数组两种形态都兼容。
        constexpr char MESSAGE_PORT_ID[] = "message";

        // 降级日志：条目未能进用户面板时按条目级别落对应调试日志，
        // [focus-degraded] 前缀标记。文本只作 fmt 实参，不作格式串（上
        // 游文本不可信，防格式串注入）。
        void LogDegradedEntry(
            Das::ExportInterface::DasLogType entry_type,
            std::string_view                 entry_text)
        {
            const std::string degraded_text =
                DAS_FMT_NS::format("[focus-degraded] {}", entry_text);
            switch (entry_type)
            {
            case Das::ExportInterface::DAS_LOG_TYPE_ERROR:
            {
                DAS_LOG_ERROR(degraded_text.c_str());
                break;
            }
            case Das::ExportInterface::DAS_LOG_TYPE_WARNING:
            {
                DAS_LOG_WARNING(degraded_text.c_str());
                break;
            }
            default:
            {
                DAS_LOG_INFO(degraded_text.c_str());
                break;
            }
            }
        }
    } // namespace

    GraphTaskFocusLogComponent::GraphTaskFocusLogComponent(
        Das::ExportInterface::IDasProcessLogRecorder* injected_recorder)
        : injected_recorder_(injected_recorder),
          settings_(Das::Utils::MakeYyjsonObject())
    {
    }

    Das::ExportInterface::DasLogType
    GraphTaskFocusLogComponent::DeriveEntryType(
        bool error_checked,
        bool warning_checked)
    {
        if (error_checked)
        {
            return Das::ExportInterface::DAS_LOG_TYPE_ERROR;
        }
        if (warning_checked)
        {
            return Das::ExportInterface::DAS_LOG_TYPE_WARNING;
        }
        return Das::ExportInterface::DAS_LOG_TYPE_INFO;
    }

    bool GraphTaskFocusLogComponent::FindSettingsBool(
        std::string_view key) const
    {
        const auto settings_object = settings_.as_object();
        if (!settings_object.has_value() || !settings_object->contains(key))
        {
            return false;
        }
        const auto value = (*settings_object)[key].as_bool();
        return value.has_value() ? *value : false;
    }

    DasResult GraphTaskFocusLogComponent::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        // 组件身份 = manifest 组件 GUID（单一事实源在 FOCUS_LOG_-
        // COMPONENT_GUID_TEXT 常量）；类 GUID 仅供 ImplBase 模板实例化，
        // 不对外。string_view 无终止符保证，先物化再取 c_str()。
        const std::string guid_text{FOCUS_LOG_COMPONENT_GUID_TEXT};
        return DasMakeDasGuid(guid_text.c_str(), p_out_guid);
    }

    DasResult GraphTaskFocusLogComponent::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.Plugins.DasGraphTask.GraphTaskFocusLogComponent",
            pp_out_name);
    }

    DasResult GraphTaskFocusLogComponent::ApplySettingsChange(
        Das::ExportInterface::IDasJson*  p_request_json,
        Das::ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        if (p_request_json != nullptr)
        {
            const auto request = Das::Utils::ReadJson(p_request_json);
            if (request.has_value() && request->is_object())
            {
                const auto request_object = request->as_object();
                if (request_object.has_value()
                    && request_object->contains(std::string_view("settings")))
                {
                    // GraphRuntime 投递的 {"settings": S} 包装：S 是节点
                    // 当前配置的全量投影——替换语义而非逐键合并；S 为
                    // null 保持现值。非对象/解析失败静默保持现值。
                    const auto& inner =
                        (*request_object)[std::string_view("settings")];
                    if (!inner.is_null())
                    {
                        settings_ = Das::Utils::CloneJson(inner);
                    }
                }
            }
        }

        *pp_out_result_json = nullptr;
        return DAS_S_OK;
    }

    DasResult GraphTaskFocusLogComponent::Do(
        Das::PluginInterface::IDasStopToken*       stop_token,
        Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        Das::ExportInterface::IDasPortMap**        pp_out_port_map)
    {
        if (pp_out_port_map == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_port_map = nullptr;

        // 纯落盘原语：无等待无子调用，不轮询取消。
        (void)stop_token;

        const auto entry_type = DeriveEntryType(
            FindSettingsBool("error"),
            FindSettingsBool("warning"));

        // message 口：缺键/NULL 均为无值流入（跳过记录）；STRING 取文本
        // ——空串也是合法值照常记录；其它类型是连线类型错误的防御位（编
        // 译期边校验拦截，此处仅降级提示不失败）。
        bool        has_message_text = false;
        std::string message_text;
        if (p_input_port_map != nullptr)
        {
            const DasReadOnlyString message_key{MESSAGE_PORT_ID};
            bool                    message_port_present = false;
            if (DAS::IsOk(p_input_port_map->Has(
                    message_key.Get(),
                    &message_port_present))
                && message_port_present)
            {
                Das::ExportInterface::DasVariantType kind =
                    Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
                if (DAS::IsOk(
                        p_input_port_map->GetType(message_key.Get(), &kind)))
                {
                    if (kind == Das::ExportInterface::DAS_VARIANT_TYPE_STRING)
                    {
                        message_text = Das::Utils::GetPortString(
                            p_input_port_map,
                            MESSAGE_PORT_ID);
                        has_message_text = true;
                    }
                    else if (
                        kind != Das::ExportInterface::DAS_VARIANT_TYPE_NULL)
                    {
                        LogDegradedEntry(
                            entry_type,
                            "focus-log message port is not a string; entry "
                            "skipped");
                    }
                }
            }
        }

        if (has_message_text)
        {
            // recorder 获取：注入缝优先（测试持引用保活）；生产经主进程
            // 具名服务 C ABI resolve——inline thread_local 跨 DLL 分裂恒
            // 空，禁用 GetCurrentIpcContext（项目铁律）。resolve/Record
            // 失败均按条目级别降级，节点仍成功。
            Das::ExportInterface::IDasProcessLogRecorder* recorder =
                injected_recorder_;
            if (recorder == nullptr)
            {
                DasPtr<Das::ExportInterface::IDasProcessLogRecorder> resolved;
                DasPtr<IDasBase> resolved_base;
                // 主进程具名服务（IPC 模式经 proxy 回主进程写
                // user.jsonl）；具名字面量与 GraphTaskFocusLog 生产入
                // 口一致。
                const DasResult resolve_rc = DasQueryMainProcessInterfaceByName(
                    "das.focus-log-recorder",
                    resolved_base.Put());
                if (DAS::IsOk(resolve_rc) && resolved_base)
                {
                    (void)resolved_base.As(resolved.Put());
                }
                recorder = resolved.Get();
                if (recorder == nullptr)
                {
                    LogDegradedEntry(entry_type, message_text);
                }
            }
            if (recorder != nullptr)
            {
                const DasResult record_rc =
                    RecordFocusLogEntry(recorder, entry_type, message_text);
                if (DAS::IsFailed(record_rc))
                {
                    LogDegradedEntry(entry_type, message_text);
                }
            }
        }

        // 无输出端口：恒产有效空 port map（begin 组件零输出同款）。
        DAS::DasPtr<Das::ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }

} // namespace Das::Plugins::DasGraphTask
