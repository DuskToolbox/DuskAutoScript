#ifndef DAS_HTTP_BEAST_JSONUTILS_HPP
#define DAS_HTTP_BEAST_JSONUTILS_HPP

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>

namespace Das::Http::Beast::JsonUtils
{

// 从JSON中获取字符串，带默认值
inline std::string GetString(const nlohmann::json& j, const std::string& key, const std::string& default_value = "")
{
    if (j.contains(key) && j[key].is_string())
    {
        return j[key].get<std::string>();
    }
    return default_value;
}

// 从JSON中获取整数，带默认值
inline int GetInt(const nlohmann::json& j, const std::string& key, int default_value = 0)
{
    if (j.contains(key) && j[key].is_number_integer())
    {
        return j[key].get<int>();
    }
    return default_value;
}

// 从JSON中获取布尔值，带默认值
inline bool GetBool(const nlohmann::json& j, const std::string& key, bool default_value = false)
{
    if (j.contains(key) && j[key].is_boolean())
    {
        return j[key].get<bool>();
    }
    return default_value;
}

// 从JSON中获取数组
template<typename T>
inline std::vector<T> GetArray(const nlohmann::json& j, const std::string& key)
{
    std::vector<T> result;
    if (j.contains(key) && j[key].is_array())
    {
        result = j[key].get<std::vector<T>>();
    }
    return result;
}

// 检查字段是否存在
inline bool HasField(const nlohmann::json& j, const std::string& key)
{
    return j.contains(key);
}

// 创建标准响应
inline nlohmann::json CreateSuccessResponse(const nlohmann::json& data = nullptr)
{
    nlohmann::json response;
    response["code"] = DAS_S_OK;
    response["message"] = "";
    response["data"] = data;
    return response;
}

// 创建错误响应
inline nlohmann::json CreateErrorResponse(DasResult error_code, const std::string& message)
{
    nlohmann::json response;
    response["code"] = error_code;
    response["message"] = message;
    response["data"] = nullptr;
    return response;
}

// IDasReadOnlyString 转 JSON
template<typename T>
inline void DasStringToJson(T* das_string, nlohmann::json& j)
{
    if (das_string)
    {
        const char* utf8_string = nullptr;
        if (das_string->GetUtf8(&utf8_string) == DAS_S_OK && utf8_string)
        {
            j = utf8_string;
        }
    }
}

// JSON 转 IDasReadOnlyString
inline DasPtr<IDasReadOnlyString> JsonToDasString(const nlohmann::json& j)
{
    if (j.is_string())
    {
        DasPtr<IDasReadOnlyString> result;
        std::string str = j.get<std::string>();
        CreateIDasReadOnlyStringFromUtf8(str.c_str(), result.Put());
        return result;
    }
    return {};
}

} // namespace Das::Http::Beast::JsonUtils

#endif // DAS_HTTP_BEAST_JSONUTILS_HPP
