#ifndef DAS_HTTP_CONTROLLER_DASMISCCONTROLLER_H
#define DAS_HTTP_CONTROLLER_DASMISCCONTROLLER_H

#include "Config.h"
#include "beast/Request.hpp"
#include "das/DasApi.h"

#include <cpp_yyjson.hpp>

namespace Das::Http
{

    class DasMiscController
    {
    public:
        Beast::HttpResponse Alive(const Beast::HttpRequest& request)
        {
            yyjson::writer::detail::value data(
                yyjson::construct_object_type_t{});
            data["alive"] = static_cast<int64_t>(1);
            return Beast::HttpResponse::CreateSuccessResponse(data);
        }

        Beast::HttpResponse RequestShutdown(const Beast::HttpRequest& request)
        {
            DAS::Http::g_server_condition.RequestServerStop();
            DAS_LOG_INFO("RequestServerStop!");
            return Beast::HttpResponse::CreateSuccessResponse();
        }
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_DASMISCCONTROLLER_H
