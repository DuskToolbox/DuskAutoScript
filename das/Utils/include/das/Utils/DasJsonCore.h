#ifndef DAS_UTILS_DASJSONCORE_H
#define DAS_UTILS_DASJSONCORE_H

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/IDasBase.h>
#include <das/Utils/Config.h>
#include <das/_autogen/idl/abi/DasJson.h>
#include <optional>
#include <string>
#include <string_view>

DAS_UTILS_NS_BEGIN

/// 将 yyjson writer value 映射到 DasType 枚举。
inline Das::ExportInterface::DasType YyjsonValueToDasType(
    const yyjson::writer::const_value_ref& v)
{
    if (v.is_null())
    {
        return Das::ExportInterface::DAS_TYPE_NULL;
    }
    if (v.is_object())
    {
        return Das::ExportInterface::DAS_TYPE_JSON_OBJECT;
    }
    if (v.is_array())
    {
        return Das::ExportInterface::DAS_TYPE_JSON_ARRAY;
    }
    if (v.is_string())
    {
        return Das::ExportInterface::DAS_TYPE_STRING;
    }
    if (v.is_bool())
    {
        return Das::ExportInterface::DAS_TYPE_BOOL;
    }
    if (v.is_uint())
    {
        return Das::ExportInterface::DAS_TYPE_UINT;
    }
    if (v.is_sint())
    {
        return Das::ExportInterface::DAS_TYPE_INT;
    }
    if (v.is_real())
    {
        return Das::ExportInterface::DAS_TYPE_FLOAT;
    }
    return Das::ExportInterface::DAS_TYPE_UNSUPPORTED;
}

/// 从 UTF-8 字符串解析 JSON。
/// @param sv 要解析的 JSON 字符串视图
/// @param flags yyjson ReadFlag 位掩码（默认无特殊标志）
/// @return 解析后的 mutable value，解析失败返回 nullopt
inline std::optional<yyjson::value> ParseYyjsonFromString(
    std::string_view sv,
    yyjson::ReadFlag flags = yyjson::ReadFlag::NoFlag)
{
    try
    {
        auto result = yyjson::read(sv, flags);
        return yyjson::value(result);
    }
    catch (const yyjson::read_error&)
    {
        return std::nullopt;
    }
}

/// 将 yyjson value 序列化为 JSON 字符串。
/// @param v 要序列化的 value
/// @param pretty 是否美化输出（2 空格缩进）
/// @return JSON 字符串，序列化失败返回 nullopt
inline std::optional<std::string> SerializeYyjsonValue(
    const yyjson::value& v,
    bool                 pretty = false)
{
    try
    {
        auto flags =
            pretty ? yyjson::WriteFlag::Pretty : yyjson::WriteFlag::NoFlag;
        auto json_str = v.write(flags);
        return std::string(json_str.data(), json_str.size());
    }
    catch (const yyjson::write_error&)
    {
        return std::nullopt;
    }
}

/// Create an empty JSON object ({}).
inline yyjson::value MakeYyjsonObject()
{
    auto result = ParseYyjsonFromString("{}");
    return result ? std::move(*result) : yyjson::value{};
}

/// Create an empty JSON array ([]).
inline yyjson::value MakeYyjsonArray()
{
    auto result = ParseYyjsonFromString("[]");
    return result ? std::move(*result) : yyjson::value{};
}

DAS_UTILS_NS_END

#endif // DAS_UTILS_DASJSONCORE_H
