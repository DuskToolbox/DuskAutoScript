#ifndef DAS_UTILS_ENUMTUILS_HPP
#define DAS_UTILS_ENUMTUILS_HPP

#include <das/DasConfig.h>
#include <das/ExportInterface/DasJson.h>
#include <das/Utils/UnexpectedEnumException.h>
#include <magic_enum.hpp>
#include <nlohmann/json.hpp>
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
Enum JsonToEnum(const nlohmann::json& json, const char* key)
{
    const auto string = json.at(key).get<std::string>();
    return StringToEnum<Enum>(string);
}

DAS_UTILS_NS_END

NLOHMANN_JSON_SERIALIZE_ENUM(
    DasType,
    {{DAS_TYPE_INT, "int"},
     {DAS_TYPE_FLOAT, "float"},
     {DAS_TYPE_STRING, "string"},
     {DAS_TYPE_BOOL, "bool"}});

#endif // DAS_UTILS_ENUMTUILS_HPP