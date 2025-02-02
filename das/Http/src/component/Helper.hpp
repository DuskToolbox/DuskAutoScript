#ifndef DAS_HTTP_COMPONENT_HELPER_HPP
#define DAS_HTTP_COMPONENT_HELPER_HPP

#include "das/Core/Exceptions/DasException.h"
#include "das/PluginInterface/IDasErrorLens.h"
#include "das/Utils/fmt.h"
#include "oatpp/web/server/api/ApiController.hpp"

#include "dto/Global.hpp"

#include <nlohmann/json.hpp>
#include <oatpp/web/protocol/http/outgoing/BufferBody.hpp>

#define DAS_HTTP_MAKE_RESPONSE(error_code)                                     \
    MakeResponse(error_code, {__FILE__, __LINE__, DAS_FUNCTION})

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

    class DasApiController : public oatpp::web::server::api::ApiController
    {
    public:
        DasApiController(
            std::shared_ptr<ObjectMapper> object_mapper =
                oatpp::parser::json::mapping::ObjectMapper::createShared())
            : ApiController{std::move(object_mapper)}
        {
        }

        auto MakeResponse(const oatpp::Void& p_response) const
        {
            return createDtoResponse(Status::CODE_200, p_response);
        }

        auto MakeResponse(const Core::DasException& ex)
        {
            const auto response = ApiResponse<void>::createShared();
            response->code = ex.GetErrorCode();
            response->message = ex.what();
            return MakeResponse(response);
        }

        auto MakeResponse(
            DasResult                        error_code,
            const DasResponseSourceLocation& source_location)
        {

            const auto response = ApiResponse<void>::createShared();
            response->code = error_code;
            const auto message = DAS_FMT_NS::format(
                "[{} {}:{}] {}.",
                source_location.function,
                source_location.file,
                source_location.line,
                GetPredefinedErrorMessage(error_code));
            response->message = std::move(message);
            return MakeResponse(response);
        }

        auto MakeSuccessResponse() const
        {
            const auto response = ApiResponse<void>::createShared();
            response->code = DAS_S_OK;
            response->message = "";
            return MakeResponse(response);
        }
    };

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
