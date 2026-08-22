#include "FlowControlI18n.h"

#include <das/DasString.hpp>

DAS_NS_BEGIN

namespace Plugins::DasFlowControl
{
    namespace
    {
        // 测试注入的替身表（持引用保活）；非空时优先于目录装载。
        DasPtr<ExportInterface::IDasI18n> test_i18n;
    } // namespace

    ExportInterface::IDasI18n* GetFlowControlI18n() { return test_i18n.Get(); }

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
