#ifndef DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP

#include "Config.h"
#include "beast/JsonUtils.hpp"
#include "beast/Request.hpp"
#include "component/Helper.hpp"
#include "das/DasApi.h"
#include "das/DasPtr.hpp"
#include "dto/Global.hpp"
#include <nlohmann/json.hpp>
#include <string>

namespace Das::Http
{

    /**
     * @brief 定义配置文件管理相关API
     * Define profile related APIs
     */
    struct DasUiSettingsController
    {
        Beast::HttpResponse V1SettingsGet(const Beast::HttpRequest& request)
        {
            return Beast::HttpResponse::CreateErrorResponse(
                DAS_E_NO_IMPLEMENTATION,
                "UI Settings get API is not implemented");
        }

        Beast::HttpResponse V1SettingsUpdate(const Beast::HttpRequest& request)
        {
            return Beast::HttpResponse::CreateErrorResponse(
                DAS_E_NO_IMPLEMENTATION,
                "UI Settings update API is not implemented");
        }
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP
