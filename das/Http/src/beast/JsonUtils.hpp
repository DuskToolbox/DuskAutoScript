#ifndef DAS_HTTP_BEAST_JSONUTILS_HPP
#define DAS_HTTP_BEAST_JSONUTILS_HPP

#include <cpp_yyjson.hpp>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/DasJsonCore.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Das::Http::Beast::JsonUtils
{

    using JsonValue = yyjson::writer::detail::value;

    // 从JSON中获取字符串，带默认值
    inline std::string GetString(
        const JsonValue&       j,
        const std::string_view key,
        const std::string&     default_value = "")
    {
        auto val = j[key];
        auto opt_str = val.as_string();
        return opt_str ? std::string(opt_str.value()) : default_value;
    }

    // 从JSON中获取整数，带默认值
    inline int64_t GetInt(
        const JsonValue&       j,
        const std::string_view key,
        int64_t                default_value = 0)
    {
        auto val = j[key];
        auto opt_int = val.as_sint();
        return opt_int ? opt_int.value() : default_value;
    }

    // 从JSON中获取布尔值，带默认值
    inline bool GetBool(
        const JsonValue&       j,
        const std::string_view key,
        bool                   default_value = false)
    {
        auto val = j[key];
        auto opt_bool = val.as_bool();
        return opt_bool ? opt_bool.value() : default_value;
    }

    // 从JSON中获取数组
    template <typename T>
    inline std::vector<T> GetArray(
        const JsonValue&       j,
        const std::string_view key)
    {
        std::vector<T> result;
        auto           val = j[key];
        auto           opt_arr = val.as_array();
        if (opt_arr)
        {
            for (auto& elem : opt_arr.value())
            {
                auto opt_v = elem.template as_sint();
                if (opt_v)
                {
                    result.push_back(static_cast<T>(opt_v.value()));
                }
            }
        }
        return result;
    }

    // 检查字段是否存在
    inline bool HasField(const JsonValue& j, const std::string_view key)
    {
        return !j[key].is_null();
    }

    // 创建标准响应
    inline JsonValue CreateSuccessResponse(const JsonValue& data = JsonValue{})
    {
        JsonValue response(yyjson::construct_object_type_t{});
        response["Code"] = static_cast<int64_t>(DAS_S_OK);
        response["Message"] = std::string{};
        response["Data"] = data;
        return response;
    }

    // 创建错误响应
    inline JsonValue CreateErrorResponse(
        DasResult          error_code,
        const std::string& message)
    {
        JsonValue response(yyjson::construct_object_type_t{});
        response["Code"] = static_cast<int64_t>(error_code);
        response["Message"] = std::string{message};
        response["Data"] = JsonValue{};
        return response;
    }

    // IDasReadOnlyString 转 JSON
    template <typename T>
    inline void DasStringToJson(T* das_string, JsonValue& j)
    {
        if (das_string)
        {
            const char* utf8_string = nullptr;
            if (das_string->GetUtf8(&utf8_string) == DAS_S_OK && utf8_string)
            {
                j = JsonValue(utf8_string);
            }
        }
    }

    // JSON 转 IDasReadOnlyString
    inline DasPtr<IDasReadOnlyString> JsonToDasString(const JsonValue& j)
    {
        auto opt_str = j.as_string();
        if (opt_str)
        {
            DasPtr<IDasReadOnlyString> result;
            std::string                str(opt_str.value());
            CreateIDasReadOnlyStringFromUtf8(str.c_str(), result.Put());
            return result;
        }
        return {};
    }

} // namespace Das::Http::Beast::JsonUtils

#endif // DAS_HTTP_BEAST_JSONUTILS_HPP
