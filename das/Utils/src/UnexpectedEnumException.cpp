#include <das/Utils/UnexpectedEnumException.h>
#include <das/Utils/fmt.h>
#include <magic_enum_format.hpp>

DAS_UTILS_NS_BEGIN

UnexpectedEnumException::UnexpectedEnumException(
    const std::string_view u8_enum_value)
    : DasException{
          DAS_E_INVALID_ENUM,
          DAS::fmt::format("Unexpected enum found. Value = {}", u8_enum_value)}
{
}

UnexpectedEnumException::UnexpectedEnumException(std::int64_t enum_value)
    : DasException{
          DAS_E_INVALID_ENUM,
          DAS::fmt::format("Unexpected enum found. Value = {}", enum_value)}
{
}

UnexpectedEnumException::UnexpectedEnumException(std::uint64_t enum_value)
    : DasException{
          DAS_E_INVALID_ENUM,
          DAS::fmt::format("Unexpected enum found. Value = {}", enum_value)}
{
}

DAS_UTILS_NS_END
