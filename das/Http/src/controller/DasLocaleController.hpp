#ifndef DAS_HTTP_CONTROLLER_DASLOCALECONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASLOCALECONTROLLER_HPP

#include "Config.h"
#include "beast/Request.hpp"
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasLocaleService.hpp>

namespace Das::Http
{

    // HTTP 入口触发主进程 das.locale 服务。PutLocale 调 SetDefaultLocale，
    // service 内部既更新主进程 GlobalLocale，又反向遍历 callbacks_ 下发子进程
    // （DasLocaleCallbackImpl）——「主进程变更→子进程同步」由既有 IPC 链路
    // 保证，HTTP 层只负责触发。
    class DasLocaleController
    {
    public:
        explicit DasLocaleController(
            Das::ExportInterface::IDasLocaleService& locale_service)
            : locale_service_(locale_service)
        {
        }

        // POST api/v1/locale/put  body: {"locale":"zh"}
        Beast::HttpResponse PutLocale(const Beast::HttpRequest& request)
        {
            const auto& body_raw = request.JsonBody();
            auto        body_obj_opt = body_raw.as_object();
            if (!body_obj_opt)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Request body must be a JSON object");
            }

            const auto& body = body_obj_opt.value();
            if (body["locale"].is_null() || !body["locale"].is_string())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Missing or invalid 'locale' field");
            }

            auto locale_opt = body["locale"].as_string();
            if (!locale_opt)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Missing or invalid 'locale' field");
            }

            const std::string          locale(locale_opt.value());
            DasPtr<IDasReadOnlyString> p_locale;
            const auto                 cr = CreateIDasReadOnlyStringFromUtf8(
                locale.c_str(),
                p_locale.Put());
            if (DAS::IsFailed(cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    cr,
                    "Failed to create locale string");
            }

            const auto result =
                locale_service_.SetDefaultLocale(p_locale.Get());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to set default locale");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        // POST api/v1/locale/get  -> {"code":0,"data":{"locale":"zh"}}
        Beast::HttpResponse GetLocale(const Beast::HttpRequest& request)
        {
            (void)request;

            DasPtr<IDasReadOnlyString> p_locale;
            const auto                 result =
                locale_service_.GetDefaultLocale(p_locale.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to get default locale");
            }
            if (!p_locale)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_FAIL,
                    "Default locale is empty");
            }

            const char* c_str = nullptr;
            const auto  get_result = p_locale->GetUtf8(&c_str);
            if (DAS::IsFailed(get_result) || c_str == nullptr)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS::IsFailed(get_result) ? get_result : DAS_E_FAIL,
                    "Failed to read locale string");
            }

            auto data = Das::Utils::MakeYyjsonObject();
            auto data_obj_opt = data.as_object();
            if (data_obj_opt)
            {
                data_obj_opt.value()["locale"] = std::string(c_str);
            }
            return Beast::HttpResponse::CreateSuccessResponse(data);
        }

    private:
        Das::ExportInterface::IDasLocaleService& locale_service_;
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_DASLOCALECONTROLLER_HPP
