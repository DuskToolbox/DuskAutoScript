#ifndef DAS_HTTP_DTO_GLOBAL_HPP
#define DAS_HTTP_DTO_GLOBAL_HPP

/**
 *  API全局类型
 *  API global type
 */

#include <cpp_yyjson.hpp>
#include <das/IDasBase.h>
#include <das/Utils/DasJsonCore.h>
#include <optional>

namespace Das::Http::Dto
{

    // 统一响应包装类型
    // Define unified response wrapper type
    template <typename T>
    struct ApiResponse
    {
        int32_t     code;
        std::string message;
        T           data;

        // 创建成功响应
        static ApiResponse<T> Success(
            const T&           data = T{},
            const std::string& message = "")
        {
            return ApiResponse<T>{DAS_S_OK, message, data};
        }

        // 创建错误响应
        static ApiResponse<T> Error(DasResult code, const std::string& message)
        {
            return ApiResponse<T>{code, message, T{}};
        }
    };

    // void特化
    template <>
    struct ApiResponse<void>
    {
        int32_t       code;
        std::string   message;
        yyjson::value data;

        static ApiResponse<void> Success(const std::string& message = "")
        {
            yyjson::value null_val{};
            return ApiResponse<void>{DAS_S_OK, message, std::move(null_val)};
        }

        static ApiResponse<void> Error(
            DasResult          code,
            const std::string& message)
        {
            yyjson::value null_val{};
            return ApiResponse<void>{code, message, std::move(null_val)};
        }
    };

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_GLOBAL_HPP
