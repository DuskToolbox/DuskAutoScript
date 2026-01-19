#ifndef DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP

#include "Config.h"
#include "beast/JsonUtils.hpp"
#include "beast/Request.hpp"
#include "component/Helper.hpp"
#include "das/DasPtr.hpp"
#include "das/ExportInterface/DasLogger.h"
#include "das/ExportInterface/IDasSettings.h"
#include "dto/Global.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace Das::Http
{

    /**
     *  @brief 定义配置文件管理相关API
     *  Define profile related APIs
     */
    struct DasUiSettingsController
    {
        Beast::HttpResponse V1SettingsGet(const Beast::HttpRequest& request)
        {
            try
            {
                DAS::DasPtr<IDasReadOnlyString> p_ui_json;
                const auto code = DasLoadExtraStringForUi(p_ui_json.Put());
                if (DAS::IsFailed(code))
                {
                    return Beast::HttpResponse::CreateErrorResponse(
                        code,
                        DAS::Http::GetPredefinedErrorMessage(code));
                }

                const auto data =
                    DAS::Http::DasString2RawString(p_ui_json.Get());
                nlohmann::json json_data;
                try
                {
                    json_data = nlohmann::json::parse(data);
                }
                catch (...)
                {
                    // If parsing fails, just return the string as-is
                    json_data = data;
                }
                return Beast::HttpResponse::CreateSuccessResponse(json_data);
            }
            catch (const DasException& ex)
            {
                const auto log_message = DAS_FMT_NS::format(
                    "Error code = {}, message = {}",
                    ex.GetErrorCode(),
                    ex.what());
                DAS_LOG_ERROR(log_message.c_str());
                return Beast::HttpResponse::CreateErrorResponse(
                    ex.GetErrorCode(),
                    ex.what());
            }
            catch (const std::exception& ex)
            {
                DAS_LOG_ERROR(ex.what());
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_UNEXPECTED,
                    ex.what());
            }
        }

        Beast::HttpResponse V1SettingsUpdate(const Beast::HttpRequest& request)
        {
            try
            {
                const auto& json_body = request.JsonBody();
                const auto  body_str = json_body.dump();

                DAS::DasPtr<IDasReadOnlyString> p_ui_json;
                DAS_THROW_IF_FAILED_EC(
                    ::CreateIDasReadOnlyStringFromUtf8(
                        body_str.c_str(),
                        p_ui_json.Put()));
                DAS_THROW_IF_FAILED_EC(
                    DasSaveExtraStringForUi(p_ui_json.Get()));

                return Beast::HttpResponse::CreateSuccessResponse(nullptr);
            }
            catch (const DasException& ex)
            {
                const auto log_message = DAS_FMT_NS::format(
                    "Error code = {}, message = {}",
                    ex.GetErrorCode(),
                    ex.what());
                DAS_LOG_ERROR(log_message.c_str());
                return Beast::HttpResponse::CreateErrorResponse(
                    ex.GetErrorCode(),
                    ex.what());
            }
            catch (const std::exception& ex)
            {
                DAS_LOG_ERROR(ex.what());
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_UNEXPECTED,
                    ex.what());
            }
        }
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP
