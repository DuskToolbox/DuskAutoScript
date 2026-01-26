#ifndef DAS_HTTP_COMPONENT_HELPER_HPP
#define DAS_HTTP_COMPONENT_HELPER_HPP

#include "beast/JsonUtils.hpp"
#include "beast/Request.hpp"
#include "das/DasException.hpp"
#include "das/Utils/fmt.h"
#include <boost/beast/core.hpp>

#include "dto/Global.hpp"
#include <das/DasApi.h>

namespace Das
{
    using DasResult = int32_t;
}

#include <nlohmann/json.hpp>

#define DAS_HTTP_MAKE_RESPONSE(error_code)                                     \
    Beast::HttpResponse::CreateErrorResponse(                                  \
        error_code,                                                            \
        Das::Http::GetPredefinedErrorMessage(error_code))

namespace Das::Http
{
    struct DasResponseSourceLocation
    {
        const char* file;
        int         line;
        const char* function;
    };

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

    inline const char* DasString2RawString(IDasReadOnlyString* p_string)
    {
        const char* p_u8_string;
        if (const auto get_result = p_string->GetUtf8(&p_u8_string);
            IsFailed(get_result))
        {
            DAS_THROW_EC(get_result);
        }
        return p_u8_string;
    }

    inline DasPtr<IDasReadOnlyString> RawString2DasString(const char* p_string)
    {
        DasPtr<IDasReadOnlyString> p_result{};
        const auto                 create_result =
            ::CreateIDasReadOnlyStringFromUtf8(p_string, p_result.Put());
        if (IsFailed(create_result))
        {
            DAS_THROW_EC(create_result);
        }
        return p_result;
    }

} // Das::Http

#endif // DAS_HTTP_COMPONENT_HELPER_HPP
