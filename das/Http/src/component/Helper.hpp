#ifndef DAS_HTTP_COMPONENT_HELPER_HPP
#define DAS_HTTP_COMPONENT_HELPER_HPP

#include <das/PluginInterface/IDasErrorLens.h>

namespace Das::Http
{

    inline std::string GetPredefinedErrorMessage(DasResult error_code)
    {
        DasPtr<IDasReadOnlyString> p_error_message{};
        if (const auto get_result =
                DasGetPredefinedErrorMessage(error_code, p_error_message.Put());
            IsFailed(get_result))
        {
            return DAS_FMT_NS::format(
                "Get predefined error message failed. Error code = {}.",
                get_result);
        }
        const char* p_u8_error_message{nullptr};
        if (const auto get_result =
                p_error_message->GetUtf8(&p_u8_error_message);
            IsFailed(get_result))
        {
            return DAS_FMT_NS::format(
                "Call GetUtf8 failed. Error code = {}.",
                get_result);
        }

        return p_u8_error_message;
    }

} // Das::Http

#endif // DAS_HTTP_COMPONENT_HELPER_HPP
