#ifndef DAS_HTTP_CONTROLLER_LOGCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_LOGCONTROLLER_HPP

#include "../component/Helper.hpp"
#include "Config.h"
#include "beast/JsonUtils.hpp"
#include "beast/Request.hpp"
#include "dto/Log.hpp"
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/DasLogger.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Das::Http
{

    // 日志接口：按字节偏移游标读取写盘的日志文件（IDasLogArchive），支持 tail /
    // after / before。
    class DasLogController
    {
        DAS::DasPtr<ExportInterface::IDasLogArchive> p_archive{};

    public:
        DasLogController()
        {
            DAS_LOG_INFO("Preparing to load log archive.");
            ::CreateIDasLogArchive(
                ExportInterface::DAS_LOG_ARCHIVE_SOURCE_USER_JSONL,
                p_archive.Put());
            DAS_LOG_INFO("Log archive loaded.");
        }

        /**
         *  定义日志相关API
         *  Define log related APIs
         */
        Beast::HttpResponse GetLogs(const Beast::HttpRequest& request)
        {
            if (request.GetPathParameter("profile") != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            using namespace Das::Http::Beast::JsonUtils;

            const auto& body = request.JsonBody();

            DAS::DasPtr<IDasReadOnlyString> lines_str;
            uint64_t                        next_offset = 0;
            uint64_t                        earliest_cursor = 0;
            bool                            eof = false;
            DasResult                       rc;

            if (HasField(body, "before"))
            {
                const auto offset =
                    static_cast<uint64_t>(GetInt(body, "before", 0));
                const auto lines =
                    static_cast<uint32_t>(GetInt(body, "lines", 200));
                rc = p_archive->ReadBefore(
                    offset,
                    lines,
                    lines_str.Put(),
                    &next_offset,
                    &eof);
            }
            else if (HasField(body, "after"))
            {
                const auto offset =
                    static_cast<uint64_t>(GetInt(body, "after", 0));
                rc =
                    p_archive->ReadAfter(offset, lines_str.Put(), &next_offset);
            }
            else
            {
                const auto lines =
                    static_cast<uint32_t>(GetInt(body, "tail", 500));
                rc = p_archive->ReadTail(
                    lines,
                    lines_str.Put(),
                    &next_offset,
                    &earliest_cursor);
            }

            Dto::Logs response;
            response.data.next_cursor = next_offset;
            response.data.earliest_cursor = earliest_cursor;
            response.data.eof = eof;

            if (DAS::IsOk(rc) && lines_str)
            {
                response.code = DAS_S_OK;
                const char* utf8 = nullptr;
                if (lines_str->GetUtf8(&utf8) == DAS_S_OK && utf8 != nullptr)
                {
                    SplitLines(utf8, response.data.logs);
                }
            }
            else
            {
                DAS_LOG_ERROR(std::to_string(rc).c_str());
                response.code = rc;
            }

            return Beast::HttpResponse::CreateSuccessResponse(response.data);
        }

    private:
        // IDasLogArchive 返回的是换行分隔的单串，这里按行拆成数组对外。
        static void SplitLines(const char* utf8, std::vector<std::string>& out)
        {
            std::string_view s(utf8);
            std::size_t      pos = 0;
            while (pos < s.size())
            {
                const auto       nl = s.find('\n', pos);
                const auto       len = nl == std::string_view::npos
                                           ? std::string_view::npos
                                           : nl - pos;
                std::string_view line = s.substr(pos, len);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                if (!line.empty())
                {
                    out.emplace_back(line);
                }
                if (nl == std::string_view::npos)
                {
                    break;
                }
                pos = nl + 1;
            }
        }
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_LOGCONTROLLER_HPP
