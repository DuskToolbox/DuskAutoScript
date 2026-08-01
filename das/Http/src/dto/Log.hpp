#ifndef DAS_HTTP_DTO_LOG_HPP
#define DAS_HTTP_DTO_LOG_HPP

#include "Global.hpp"

#include <cstdint>
#include <string>
#include <vector>

/**
 *  定义日志相关数据类型
 *  Define log related data types
 */

namespace Das::Http::Dto
{

    // 日志读取结果：logs 为按行拆分的日志文本；next_cursor 为下次增量请求的字节偏移
    // （对齐到行尾）；eof 表示上翻已到文件头（ReadBefore 用）。
    // DasHttp 默认 snake→camel 序列化（见 Http/CMakeLists.txt），故 next_cursor 输出为 "nextCursor"。
    struct LogsData
    {
        std::vector<std::string> logs;
        uint64_t                 next_cursor;
        uint64_t                 earliest_cursor;
        bool                     eof;
    };

    static_assert(
        yyjson::detail::transformed_name<1, LogsData>().view() == "nextCursor");
    static_assert(
        yyjson::detail::transformed_name<2, LogsData>().view() == "earliestCursor");
    static_assert(
        yyjson::detail::transformed_name<3, LogsData>().view() == "eof");

    using Logs = ApiResponse<LogsData>;

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_LOG_HPP
