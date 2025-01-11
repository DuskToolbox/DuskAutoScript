#ifndef DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP

#include "Config.h"
#include "component/Helper.hpp"
#include "das/DasPtr.hpp"
#include "das/ExportInterface/IDasSettings.h"
#include "dto/Global.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

/**
 *  @brief 定义配置文件管理相关API
 *  Define profile related APIs
 */
struct DasUiSettingsController final : public DAS::Http::DasApiController
{
    DasUiSettingsController(
        std::shared_ptr<ObjectMapper> object_mapper =
            oatpp::parser::json::mapping::ObjectMapper::createShared());

    ENDPOINT("POST", DAS_HTTP_API_PREFIX "settings/get", v1_settings_get)
    {
        const auto response = ApiResponse<String>::createShared();

        DAS::DasPtr<IDasReadOnlyString> p_ui_json;
        response->code = DasLoadExtraStringForUi(p_ui_json.Put());
        if (DAS::IsFailed(response->code))
        {
            response->message =
                DAS::Http::GetPredefinedErrorMessage(response->code);
            return MakeResponse(response);
        }

        try
        {
            response->data = DAS::Http::DasString2RawString(p_ui_json.Get());
            return MakeResponse(response);
        }
        catch (const DAS::Core::DasException& ex)
        {
            const auto log_message = DAS_FMT_NS::format(
                "Error code = {}, message = {}",
                ex.GetErrorCode(),
                ex.what());
            DAS_LOG_ERROR(log_message.c_str());
            return MakeResponse(ex);
        }
    }

    ENDPOINT(
        "POST",
        DAS_HTTP_API_PREFIX "settings/update",
        v1_settings_update,
        BODY_STRING(String, body))
    {
        try
        {
            if (!body.get())
            {
                DAS_THROW_EC(DAS_E_INVALID_STRING);
            }
            DAS::DasPtr<IDasReadOnlyString> p_ui_json;
            DAS_THROW_IF_FAILED_EC(::CreateIDasReadOnlyStringFromUtf8(
                body.get()->c_str(),
                p_ui_json.Put()));
            DAS_THROW_IF_FAILED_EC(DasSaveExtraStringForUi(p_ui_json.Get()));
        }
        catch (const DAS::Core::DasException& ex)
        {
            const auto log_message = DAS_FMT_NS::format(
                "Error code = {}, message = {}",
                ex.GetErrorCode(),
                ex.what());
            DAS_LOG_ERROR(log_message.c_str());
            return MakeResponse(ex);
        }
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif // DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP
