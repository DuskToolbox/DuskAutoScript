#ifndef DAS_HTTP_CONTROLLER_DASMISCCONTROLLER_H
#define DAS_HTTP_CONTROLLER_DASMISCCONTROLLER_H

#include "Config.h"
#include "../component/Helper.hpp"
#include "beast/Request.hpp"
#include "beast/JsonUtils.hpp"
#include "das/ExportInterface/DasLogger.h"
#include <nlohmann/json.hpp>

namespace Das::Http
{

class DasMiscController
{
public:
    Beast::HttpResponse Alive(const Beast::HttpRequest& request)
    {
        nlohmann::json data;
        data["alive"] = 1;
        return Beast::HttpResponse::CreateSuccessResponse(data);
    }

    Beast::HttpResponse RequestShutdown(const Beast::HttpRequest& request)
    {
        DAS::Http::g_server_condition.RequestServerStop();
        DAS_LOG_INFO("RequestServerStop!");
        return Beast::HttpResponse::CreateSuccessResponse(nullptr);
    }
};

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_DASMISCCONTROLLER_H
