#include "FlowControlI18n.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/BoostPathBridge.h>
#include <das/Utils/PathText.h>

#include <boost/dll/runtime_symbol_info.hpp>

#include <filesystem>

DAS_NS_BEGIN

namespace Plugins::DasFlowControl
{
    namespace
    {
        // 测试注入的替身表（持引用保活）；非空时优先于目录装载。
        DasPtr<ExportInterface::IDasI18n> test_i18n;

        // <plugin_dir>/i18n/DasFlowControl 经 boost::dll::this_line_location()
        // 推导（cwd != 插件安装目录；每插件子目录避免与 DasMaaPi 的
        // plugins/i18n/en.json 同名互覆）。boost path 不直接进窄串，经
        // helper 桥全程保 path 后再物化 UTF-8。
        bool LoadI18nFromPluginDirectory(DasPtr<ExportInterface::IDasI18n>& out)
        {
            try
            {
                const auto plugin_module = boost::dll::this_line_location();
                const auto dir_path =
                    Das::Utils::ToStdPath(plugin_module.parent_path())
                    / DAS_PATH_LITERAL("i18n")
                    / DAS_PATH_LITERAL("DasFlowControl");
                const std::string dir_utf8 = Das::Utils::PathToUtf8(dir_path);

                DasPtr<IDasReadOnlyString> dir_str;
                if (DAS::IsFailed(CreateIDasReadOnlyStringFromUtf8(
                        dir_utf8.c_str(),
                        dir_str.Put())))
                {
                    return false;
                }
                return DAS::IsOk(CreateIDasI18nFromDirectory(
                           dir_str.Get(),
                           out.Put()))
                       && out != nullptr;
            }
            catch (...)
            {
                return false;
            }
        }
    } // namespace

    ExportInterface::IDasI18n* GetFlowControlI18n()
    {
        if (test_i18n != nullptr)
        {
            return test_i18n.Get();
        }

        // 函数局部 magic static（单段式，初始化线程安全）：首次调用装载，
        // 此后复用；装载失败为空 → 每次返回 nullptr（永久降级硬编码英文，
        // 不重试）。
        static const DasPtr<ExportInterface::IDasI18n> loaded_i18n = []()
        {
            DasPtr<ExportInterface::IDasI18n> result;
            LoadI18nFromPluginDirectory(result);
            return result;
        }();
        return loaded_i18n.Get();
    }

    void SetFlowControlI18nForTest(ExportInterface::IDasI18n* i18n)
    {
        test_i18n = DasPtr<ExportInterface::IDasI18n>{i18n};
    }

    std::string TranslateFlowControlKey(
        const char*      key,
        std::string_view fallback_text)
    {
        auto* const i18n = GetFlowControlI18n();
        if (i18n == nullptr || key == nullptr)
        {
            return std::string{fallback_text};
        }

        DasPtr<IDasReadOnlyString> key_str;
        if (DAS::IsFailed(CreateIDasReadOnlyStringFromUtf8(key, key_str.Put())))
        {
            return std::string{fallback_text};
        }

        DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(i18n->Translate(key_str.Get(), text.Put()))
            || text == nullptr)
        {
            return std::string{fallback_text};
        }

        const char* utf8 = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return std::string{fallback_text};
        }
        return std::string{utf8};
    }
}

DAS_NS_END
