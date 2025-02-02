#ifndef DAS_HTTP_CONTROLLER_LOGCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_LOGCONTROLLER_HPP

#include "Config.h"

#include "../component/DasHttpLogReader.h"
#include "../component/Helper.hpp"

#include "oatpp/core/base/Environment.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include "dto/Log.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

class DasLogController final : public DAS::Http::DasApiController
{
    DAS::DasPtr<DasHttpLogReader> p_reader{DAS::MakeDasPtr<DasHttpLogReader>()};
    DAS::DasPtr<IDasLogRequester> p_requester{};

    constexpr static auto DAS_HTTP_LOG_THEME = "DuskAutoScriptHttpLogger";

public:
    DasLogController()
    {
        constexpr auto LOG_RING_BUFFER_SIZE = 64;
        ::CreateIDasLogRequester(LOG_RING_BUFFER_SIZE, p_requester.Put());
    }

    /**
     *  定义日志相关API
     *  Define log related APIs
     */
    ENDPOINT("POST", DAS_HTTP_API_PREFIX "logs", get_logs)
    {

        auto response = Logs::createShared();
        response->code = DAS_S_OK;
        response->message = "";
        response->data = LogsData::createShared();
        response->data->logs = {};

        OATPP_LOGI(DAS_HTTP_LOG_THEME, "Preparing to load logger.");

        while (true)
        {
            if (const auto error_code = p_requester->RequestOne(p_reader.Get());
                error_code == DAS_S_OK)
            {
                response->data->logs->emplace_back(p_reader->GetLog().data());
            }
            else if (error_code == DAS_E_OUT_OF_RANGE)
            {
                // 正常退出，无需额外log
                response->code = DAS_S_OK;
                break;
            }
            else
            {
                OATPP_LOGD(
                    DAS_HTTP_LOG_THEME,
                    std::to_string(error_code).c_str());
                response->code = error_code;
                break;
            }
        }

        OATPP_LOGI(DAS_HTTP_LOG_THEME, "Logger loaded.");

        return createDtoResponse(Status::CODE_200, response);
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif // DAS_HTTP_CONTROLLER_LOGCONTROLLER_HPP
