#include "GraphTaskFocusLog.h"

#include <boost/dll.hpp>
#include <das/DasApi.h>
#include <das/DasString.hpp>
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

        // DLL 自定位（DasMaaPi PluginUtils 同款）：this_line_location 返回
        // 本代码所在模块路径，i18n 表随插件部署在 DLL 目录 i18n/ 下。
        std::optional<std::filesystem::path> PluginModuleI18nDirectory()
        {
            try
            {
                const boost::filesystem::path module_path =
                    boost::dll::this_line_location();
#ifdef _WIN32
                return std::filesystem::path{
                           module_path.parent_path().wstring()}
                       / L"i18n";
#else
                return std::filesystem::path{module_path.parent_path().string()}
                       / "i18n";
#endif
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        // i18n 目录 → 完成文案模板（Translate 读 GlobalLocale）；目录缺失/
        // 工厂失败/key miss 全部回落 en 字面模板。
        std::string ResolveFinishedTextTemplate(
            const std::filesystem::path& i18n_directory)
        {
            const auto        u8_directory = i18n_directory.u8string();
            const std::string directory_utf8(
                u8_directory.begin(),
                u8_directory.end());
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
                        DasPtr<IDasReadOnlyString> key_string;
                        DasPtr<IDasReadOnlyString> translated;
                        if (DAS::IsOk(CreateIDasReadOnlyStringFromUtf8(
                                FINISHED_I18N_KEY.data(),
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
            return std::string(FINISHED_TEXT_FALLBACK);
        }
    } // namespace

    DasResult RecordGraphTaskFinishedFocusEntry(
        Das::ExportInterface::IDasProcessLogRecorder* recorder,
        const std::filesystem::path&                  i18n_directory,
        std::int64_t                                  end_value)
    {
        if (recorder == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        const std::string template_text =
            ResolveFinishedTextTemplate(i18n_directory);
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
        DasPtr<Das::ExportInterface::IDasLogEntry> entry;
        DasResult hr = CreateIDasLogEntry(entry.Put());
        if (DAS::IsFailed(hr) || !entry)
        {
            return DAS::IsFailed(hr) ? hr : DAS_E_FAIL;
        }
        hr = entry->SetType(ExportInterface::DAS_LOG_TYPE_SUCCESS);
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        DasPtr<IDasReadOnlyString> entry_text_string;
        hr = CreateIDasReadOnlyStringFromUtf8(
            entry_text.c_str(),
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

    void PublishGraphTaskFinishedFocusEntry(std::int64_t end_value)
    {
        // 主进程具名服务优先（IPC 模式经 proxy 回主进程写 user.jsonl）；
        // resolve 失败降级本地工厂——无 session 时 Record 返 OBJECT_NOT_INIT。
        // 降级与失败均落调试日志（focus 面板看不到完成事件时按此定位），
        // 任务返回码不受影响。
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
                        "(resolve_rc={}); falling back to local factory",
                        static_cast<std::int64_t>(resolve_rc))
                        .c_str());
            }
        }
        if (!recorder)
        {
            if (DAS::IsFailed(CreateIDasProcessLogRecorder(
                    nullptr,
                    nullptr,
                    recorder.Put()))
                || !recorder)
            {
                DAS_LOG_ERROR(
                    "GraphTask focus recorder unavailable; finished entry "
                    "not published");
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

} // namespace Das::Plugins::DasGraphTask
