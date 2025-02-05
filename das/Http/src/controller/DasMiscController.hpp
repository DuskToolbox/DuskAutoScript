#ifndef DAS_HTTP_CONTROLLER_DASMISCCONTROLLER_H
#define DAS_HTTP_CONTROLLER_DASMISCCONTROLLER_H

#include "../component/Helper.hpp"
#include "Config.h"
#include "das/ExportInterface/DasLogger.h"

#include OATPP_CODEGEN_BEGIN(ApiController)

class DasMiscController final : public DAS::Http::DasApiController
{
    ENDPOINT("POST", DAS_HTTP_API_PREFIX "alive", get_alive)
    {
        nlohmann::json response;
        response["code"] = 0;
        response["message"] = "";
        response["data"]["alive"] = 1;
        return oatpp::web::protocol::http::outgoing::Response::createShared(
            Status::CODE_200,
            oatpp::web::protocol::http::outgoing::BufferBody::createShared(
                String{response.dump()},
                "application/json"));
    }

    ENDPOINT("POST", DAS_HTTP_API_PREFIX "request_shutdown", request_shutdown)
    {
        DAS::Http::g_server_condition.RequestServerStop();
        DAS_LOG_INFO("RequestServerStop!");
        return MakeSuccessResponse();
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif // DAS_HTTP_CONTROLLER_DASMISCCONTROLLER_H
