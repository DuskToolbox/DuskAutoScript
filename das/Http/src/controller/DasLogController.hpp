#ifndef DAS_HTTP_CONTROLLER_LOGCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_LOGCONTROLLER_HPP

#include "../component/DasHttpLogReader.h"
#include "../component/Helper.hpp"
#include "Config.h"
#include "beast/JsonUtils.hpp"
#include "beast/Request.hpp"
#include "dto/Log.hpp"
#include <das/_autogen/idl/abi/DasLogger.h>

#include <string>

namespace Das::Http
{

    constexpr static auto DAS_HTTP_LOG_THEME = "DuskAutoScriptHttpLogger";

    class DasLogController
    {
        DAS::DasPtr<DasHttpLogReader> p_reader{
            DAS::MakeDasPtr<DasHttpLogReader>()};
        DAS::DasPtr<ExportInterface::IDasLogRequester> p_requester{};

    public:
        DasLogController()
        {
            constexpr auto LOG_RING_BUFFER_SIZE = 64;

            DAS_LOG_INFO("Preparing to load logger.");
            ::CreateIDasLogRequester(LOG_RING_BUFFER_SIZE, p_requester.Put());
            DAS_LOG_INFO("Logger loaded.");
        }

        /**
         *  定义日志相关API
         *  Define log related APIs
         */
        Beast::HttpResponse GetLogs(const Beast::HttpRequest& request)
        {
            Dto::Logs response;
            response.code = DAS_S_OK;
            response.message = "";
            response.data.logs = {};

            while (true)
            {
                if (const auto error_code =
                        p_requester->RequestOne(p_reader.Get());
                    error_code == DAS_S_OK)
                {
                    response.data.logs.emplace_back(p_reader->GetLog().data());
                }
                else if (error_code == DAS_E_OUT_OF_RANGE)
                {
                    // 正常退出，无需额外log
                    response.code = DAS_S_OK;
                    break;
                }
                else
                {
                    DAS_LOG_ERROR(std::to_string(error_code).c_str());
                    response.code = error_code;
                    break;
                }
            }

            return Beast::HttpResponse::CreateSuccessResponse(
                response.data.ToJson());
        }
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_LOGCONTROLLER_HPP
