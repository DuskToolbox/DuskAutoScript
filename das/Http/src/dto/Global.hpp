#ifndef DAS_HTTP_DTO_GLOBAL_HPP
#define DAS_HTTP_DTO_GLOBAL_HPP

/**
 *  API全局类型
 *  API global type
 */

#include <das/IDasBase.h>
#include <nlohmann/json.hpp>
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
        nlohmann::json ToJson() const
        {
            nlohmann::json j;
            j["code"] = code;
            j["message"] = message;
            if constexpr (std::is_same_v<T, nlohmann::json>)
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
        static ApiResponse<T> FromJson(const nlohmann::json& j)
        {
            ApiResponse<T> response;
            response.code = j.value("code", 0);
            response.message = j.value("message", "");
            if constexpr (std::is_same_v<T, nlohmann::json>)
            {
                response.data = j.value("data", nlohmann::json());
            }
            else
            {
                response.data = j.value("data", T{});
            }
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
        int32_t        code;
        std::string    message;
        nlohmann::json data;

        nlohmann::json ToJson() const
        {
            nlohmann::json j;
            j["code"] = code;
            j["message"] = message;
            j["data"] = data;
            return j;
        }

        static ApiResponse<void> Success(const std::string& message = "")
        {
            return ApiResponse<void>{DAS_S_OK, message, nullptr};
        }

        static ApiResponse<void> Error(
            DasResult          code,
            const std::string& message)
        {
            return ApiResponse<void>{code, message, nullptr};
        }
    };

} // namespace Das::Http::Dto

#endif // DAS_HTTP_DTO_GLOBAL_HPP