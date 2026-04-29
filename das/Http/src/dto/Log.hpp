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

        yyjson::writer::detail::value ToJson() const
        {
            auto j = Das::Utils::MakeYyjsonObject();
            auto arr = Das::Utils::MakeYyjsonArray();
            auto arr_obj_opt = arr.as_array();
            if (arr_obj_opt)
            {
                auto& arr_obj = arr_obj_opt.value();
                for (const auto& log : logs)
                {
                    arr_obj.emplace_back(std::string{log});
                }
            }
            auto obj_opt = j.as_object();
            if (obj_opt)
            {
                obj_opt.value()["logs"] = std::move(arr);
            }
            return j;
        }

        static LogsData FromJson(const yyjson::writer::detail::value& j)
        {
            LogsData data;
            auto     obj_opt = j.as_object();
            if (obj_opt)
            {
                const auto& obj = obj_opt.value();
                auto        logs_val = obj["logs"];
                auto        logs_opt = logs_val.as_array();
                if (logs_opt)
                {
                    for (const auto& item : logs_opt.value())
                    {
                        auto str_opt = item.as_string();
                        if (str_opt)
                        {
                            data.logs.push_back(std::string(str_opt.value()));
                        }
                    }
                }
            }
            return data;
        }
    };

    using Logs = ApiResponse<LogsData>;

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_LOG_HPP
