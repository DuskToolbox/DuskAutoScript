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
    
    nlohmann::json ToJson() const
    {
        nlohmann::json j;
        j["logs"] = logs;
        return j;
    }
    
    static LogsData FromJson(const nlohmann::json& j)
    {
        LogsData data;
        if (j.contains("logs") && j["logs"].is_array())
        {
            data.logs = j["logs"].get<std::vector<std::string>>();
        }
        return data;
    }
};

using Logs = ApiResponse<LogsData>;

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_LOG_HPP