#include "GraphTaskFocusLog.h"

#include <boost/dll.hpp>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/BoostPathBridge.h>
#include <das/Utils/PathText.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasI18n.h>

#include <optional>
#include <string>
#include <string_view>

namespace Das::Plugins::DasGraphTask
{
    namespace
    {
        constexpr std::string_view FINISHED_I18N_KEY = "graphTask.finished";

        // i18n 表不可用时的 en 字面模板（base locale 语义，含 {0} 占位符）。
        constexpr std::string_view FINISHED_TEXT_FALLBACK =
            "Graph task finished: end value = {0}";

        constexpr std::string_view FAILED_I18N_KEY = "graphTask.failed";

        constexpr std::string_view FAILED_TEXT_FALLBACK =
            "Graph task failed: {0}";

        constexpr std::string_view SCHEDULE_SKIPPED_I18N_KEY =
            "graphTask.scheduleSkipped";

        // i18n 表不可用时的 en 字面模板（含 {0}=跳过槽数占位符）。
        constexpr std::string_view SCHEDULE_SKIPPED_TEXT_FALLBACK =
            "Graph task missed {0} scheduled slot(s); next run moved to the "
            "upcoming slot";

        // DLL 自定位（DasMaaPi PluginUtils 同款）：this_line_location 返回
        // 本代码所在模块路径，i18n 表随插件部署在 DLL 目录 i18n/ 下。
        std::optional<std::filesystem::path> PluginModuleI18nDirectory()
        {
            try
            {
                const boost::filesystem::path module_path =
                    boost::dll::this_line_location();
                return Das::Utils::ToStdPath(module_path.parent_path())
                       / DAS_PATH_LITERAL("i18n");
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        // i18n 目录 → 文案模板（Translate 读 GlobalLocale）；目录缺失/
        // 工厂失败/key miss 全部回落 en 字面模板。
        std::string ResolveTextTemplate(
            const std::filesystem::path& i18n_directory,
            std::string_view             i18n_key,
            std::string_view             fallback_template)
        {
            const std::string directory_utf8 =
                Das::Utils::PathToUtf8(i18n_directory);
            if (!directory_utf8.empty())
            {
                DasPtr<IDasReadOnlyString> directory_string;
                if (DAS::IsOk(CreateIDasReadOnlyStringFromUtf8(
                        directory_utf8.c_str(),
                        directory_string.Put()))
                    && directory_string)
                {
                    DasPtr<Das::ExportInterface::IDasI18n> i18n;
                    if (DAS::IsOk(CreateIDasI18nFromDirectory(
                            directory_string.Get(),
                            i18n.Put()))
                        && i18n)
                    {
                        const std::string          key_text{i18n_key};
                        DasPtr<IDasReadOnlyString> key_string;
                        DasPtr<IDasReadOnlyString> translated;
                        if (DAS::IsOk(CreateIDasReadOnlyStringFromUtf8(
                                key_text.c_str(),
                                key_string.Put()))
                            && DAS::IsOk(i18n->Translate(
                                key_string.Get(),
                                translated.Put()))
                            && translated)
                        {
                            const char* utf8 = nullptr;
                            if (DAS::IsOk(translated->GetUtf8(&utf8))
                                && utf8 != nullptr && *utf8 != 0)
                            {
                                return std::string(utf8);
                            }
                        }
                    }
                }
            }
            return std::string(fallback_template);
        }
    } // namespace

    DasResult RecordFocusLogEntry(
        Das::ExportInterface::IDasProcessLogRecorder* recorder,
        Das::ExportInterface::DasLogType              entry_type,
        std::string_view                              entry_text)
    {
        if (recorder == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        // string_view 无生命周期与终止符保证：先物化具名局部（裸 c_str()
        // 只对 std::string 合法）。
        const std::string                          owned_text{entry_text};
        DasPtr<Das::ExportInterface::IDasLogEntry> entry;
        DasResult hr = CreateIDasLogEntry(entry.Put());
        if (DAS::IsFailed(hr) || !entry)
        {
            return DAS::IsFailed(hr) ? hr : DAS_E_FAIL;
        }
        hr = entry->SetType(entry_type);
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        DasPtr<IDasReadOnlyString> entry_text_string;
        hr = CreateIDasReadOnlyStringFromUtf8(
            owned_text.c_str(),
            entry_text_string.Put());
        if (DAS::IsFailed(hr) || !entry_text_string)
        {
            return DAS::IsFailed(hr) ? hr : DAS_E_FAIL;
        }
        hr = entry->AddText(entry_text_string.Get());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        return recorder->Record(entry.Get());
    }

    DasResult RecordGraphTaskFinishedFocusEntry(
        Das::ExportInterface::IDasProcessLogRecorder* recorder,
        const std::filesystem::path&                  i18n_directory,
        std::int64_t                                  end_value)
    {
        if (recorder == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        const std::string template_text = ResolveTextTemplate(
            i18n_directory,
            FINISHED_I18N_KEY,
            FINISHED_TEXT_FALLBACK);
        std::string entry_text = template_text;
        try
        {
            // make_format_args 持引用而非拥有：end_value 是具名形参，生命
            // 周期覆盖 vformat 调用（禁临时量悬垂引用）。
            entry_text = DAS_FMT_NS::vformat(
                template_text,
                DAS_FMT_NS::make_format_args(end_value));
        }
        catch (...)
        {
            // 模板占位符异常（缺参/错序）按字面输出，不阻断记录。
        }
        return RecordFocusLogEntry(
            recorder,
            ExportInterface::DAS_LOG_TYPE_SUCCESS,
            entry_text);
    }

    DasResult RecordGraphTaskFailedFocusEntry(
        Das::ExportInterface::IDasProcessLogRecorder* recorder,
        const std::filesystem::path&                  i18n_directory,
        std::string_view                              failure_message)
    {
        if (recorder == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        const std::string template_text = ResolveTextTemplate(
            i18n_directory,
            FAILED_I18N_KEY,
            FAILED_TEXT_FALLBACK);
        std::string entry_text = template_text;
        try
        {
            // string_view 无生命周期保证，先物化具名局部再入
            // make_format_args（具名形参防悬垂纪律）。
            const std::string message_text{failure_message};
            entry_text = DAS_FMT_NS::vformat(
                template_text,
                DAS_FMT_NS::make_format_args(message_text));
        }
        catch (...)
        {
            // 模板占位符异常（缺参/错序）按字面输出，不阻断记录。
        }
        return RecordFocusLogEntry(
            recorder,
            ExportInterface::DAS_LOG_TYPE_ERROR,
            entry_text);
    }

    void PublishGraphTaskFinishedFocusEntry(std::int64_t end_value)
    {
        // 主进程具名服务（IPC 模式经 proxy 回主进程写 user.jsonl）；resolve
        // 失败即落调试日志（含 end value），不降级、无本地工厂——focus 面板
        // 看不到完成事件时按此定位，任务返回码不受影响。
        DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder;
        {
            DasPtr<IDasBase> resolved_base;
            const DasResult  resolve_rc = DasQueryMainProcessInterfaceByName(
                "das.focus-log-recorder",
                resolved_base.Put());
            if (DAS::IsOk(resolve_rc) && resolved_base)
            {
                (void)resolved_base.As(recorder.Put());
            }
            else
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "GraphTask focus recorder resolve failed "
                        "(resolve_rc={}); "
                        "finished entry not published (end value={})",
                        static_cast<std::int64_t>(resolve_rc),
                        end_value)
                        .c_str());
                return;
            }
        }
        const std::optional<std::filesystem::path> i18n_directory =
            PluginModuleI18nDirectory();
        const DasResult record_rc = RecordGraphTaskFinishedFocusEntry(
            recorder.Get(),
            i18n_directory.value_or(std::filesystem::path{}),
            end_value);
        if (DAS::IsFailed(record_rc))
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "GraphTask finished focus entry not recorded "
                    "(record_rc={}, end value={})",
                    static_cast<std::int64_t>(record_rc),
                    end_value)
                    .c_str());
        }
    }

    void PublishGraphTaskFailedFocusEntry(std::string_view failure_message)
    {
        // 主进程具名服务（IPC 模式经 proxy 回主进程写 user.jsonl）；resolve
        // 失败即落调试日志（含失败消息），不降级、无本地工厂——focus 面板
        // 看不到失败事件时按此定位，任务返回码不受影响。
        DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder;
        {
            DasPtr<IDasBase> resolved_base;
            const DasResult  resolve_rc = DasQueryMainProcessInterfaceByName(
                "das.focus-log-recorder",
                resolved_base.Put());
            if (DAS::IsOk(resolve_rc) && resolved_base)
            {
                (void)resolved_base.As(recorder.Put());
            }
            else
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "GraphTask focus recorder resolve failed "
                        "(resolve_rc={}); "
                        "failed entry not published (failure message={})",
                        static_cast<std::int64_t>(resolve_rc),
                        failure_message)
                        .c_str());
                return;
            }
        }
        const std::optional<std::filesystem::path> i18n_directory =
            PluginModuleI18nDirectory();
        const DasResult record_rc = RecordGraphTaskFailedFocusEntry(
            recorder.Get(),
            i18n_directory.value_or(std::filesystem::path{}),
            failure_message);
        if (DAS::IsFailed(record_rc))
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "GraphTask failed focus entry not recorded "
                    "(record_rc={})",
                    static_cast<std::int64_t>(record_rc))
                    .c_str());
        }
    }

    DasResult RecordGraphTaskScheduleSkippedFocusEntry(
        Das::ExportInterface::IDasProcessLogRecorder* recorder,
        const std::filesystem::path&                  i18n_directory,
        std::int64_t                                  skipped_slots)
    {
        if (recorder == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        const std::string template_text = ResolveTextTemplate(
            i18n_directory,
            SCHEDULE_SKIPPED_I18N_KEY,
            SCHEDULE_SKIPPED_TEXT_FALLBACK);
        std::string entry_text = template_text;
        try
        {
            // make_format_args 持引用而非拥有：skipped_slots 是具名形参，
            // 生命周期覆盖 vformat 调用（禁临时量悬垂引用）。
            entry_text = DAS_FMT_NS::vformat(
                template_text,
                DAS_FMT_NS::make_format_args(skipped_slots));
        }
        catch (...)
        {
            // 模板占位符异常（缺参/错序）按字面输出，不阻断记录。
        }
        return RecordFocusLogEntry(
            recorder,
            ExportInterface::DAS_LOG_TYPE_WARNING,
            entry_text);
    }

    void PublishGraphTaskScheduleSkippedFocusEntry(std::int64_t skipped_slots)
    {
        // 主进程具名服务（IPC 模式经 proxy 回主进程写 user.jsonl）；resolve
        // 失败即落调试日志（含跳过槽数），不降级、无本地工厂——focus 面板
        // 看不到跳槽事件时按此定位，GetNextExecutionTime 返回码不受影响。
        DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder;
        {
            DasPtr<IDasBase> resolved_base;
            const DasResult  resolve_rc = DasQueryMainProcessInterfaceByName(
                "das.focus-log-recorder",
                resolved_base.Put());
            if (DAS::IsOk(resolve_rc) && resolved_base)
            {
                (void)resolved_base.As(recorder.Put());
            }
            else
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "GraphTask focus recorder resolve failed "
                        "(resolve_rc={}); "
                        "schedule-skipped entry not published (skipped "
                        "slots={})",
                        static_cast<std::int64_t>(resolve_rc),
                        skipped_slots)
                        .c_str());
                return;
            }
        }
        const std::optional<std::filesystem::path> i18n_directory =
            PluginModuleI18nDirectory();
        const DasResult record_rc = RecordGraphTaskScheduleSkippedFocusEntry(
            recorder.Get(),
            i18n_directory.value_or(std::filesystem::path{}),
            skipped_slots);
        if (DAS::IsFailed(record_rc))
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "GraphTask schedule-skipped focus entry not recorded "
                    "(record_rc={}, skipped slots={})",
                    static_cast<std::int64_t>(record_rc),
                    skipped_slots)
                    .c_str());
        }
    }

} // namespace Das::Plugins::DasGraphTask
