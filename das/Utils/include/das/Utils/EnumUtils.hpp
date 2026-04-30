#ifndef DAS_UTILS_ENUMTUILS_HPP
#define DAS_UTILS_ENUMTUILS_HPP

#include <cpp_yyjson.hpp>
#include <das/DasConfig.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/UnexpectedEnumException.h>
#include <das/_autogen/idl/abi/DasJson.h>
#include <magic_enum.hpp>
#include <string_view>

DAS_UTILS_NS_BEGIN

template <class Enum>
Enum StringToEnum(const std::string_view string)
{
    const auto opt_value = magic_enum::enum_cast<Enum>(string);
    if (!opt_value)
    {
        throw UnexpectedEnumException(string);
    }
    return opt_value.value();
}

template <class Enum>
Enum JsonToEnum(const yyjson::writer::const_value_ref& json, const char* key)
{
    auto obj = json.as_object();
    if (!obj)
    {
        throw UnexpectedEnumException("JSON value is not an object");
    }
    auto val = (*obj)[std::string_view(key)];
    auto str = val.as_string();
    if (!str)
    {
        throw UnexpectedEnumException(
            std::string("Missing or invalid key: ") + key);
    }
    return StringToEnum<Enum>(*str);
}

DAS_UTILS_NS_END

#endif // DAS_UTILS_ENUMTUILS_HPP