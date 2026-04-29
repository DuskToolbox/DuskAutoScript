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

        // 转换为JSON
        yyjson::writer::detail::value ToJson() const
        {
            yyjson::writer::detail::value j(yyjson::construct_object_type_t{});
            j["code"] = static_cast<int64_t>(code);
            j["message"] = std::string{message};
            if constexpr (std::is_same_v<T, yyjson::writer::detail::value>)
            {
                j["data"] = data;
            }
            else
            {
                j["data"] = data;
            }
            return j;
        }

        // 从JSON构造
        static ApiResponse<T> FromJson(const yyjson::writer::detail::value& j)
        {
            ApiResponse<T> response;
            auto           code_val = j["code"];
            auto           code_opt = code_val.as_sint();
            response.code =
                code_opt ? static_cast<int32_t>(code_opt.value()) : 0;
            auto msg_val = j["message"];
            auto msg_opt = msg_val.as_string();
            response.message = msg_opt ? std::string(msg_opt.value()) : "";
            return response;
        }

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
        int32_t                       code;
        std::string                   message;
        yyjson::writer::detail::value data;

        yyjson::writer::detail::value ToJson() const
        {
            yyjson::writer::detail::value j(yyjson::construct_object_type_t{});
            j["code"] = static_cast<int64_t>(code);
            j["message"] = std::string{message};
            j["data"] = data;
            return j;
        }

        static ApiResponse<void> Success(const std::string& message = "")
        {
            return ApiResponse<void>{DAS_S_OK, message, {}};
        }

        static ApiResponse<void> Error(
            DasResult          code,
            const std::string& message)
        {
            return ApiResponse<void>{code, message, {}};
        }
    };

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_GLOBAL_HPP
