#ifndef DAS_HTTP_DTO_LOG_HPP
#define DAS_HTTP_DTO_LOG_HPP

#include "Global.hpp"

/**
 *  定义日志相关数据类型
 *  Define log related data types
 */

namespace Das::Http::Dto
{

    struct LogsData
    {
        std::vector<std::string> logs;
    };

    using Logs = ApiResponse<LogsData>;

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_LOG_HPP
