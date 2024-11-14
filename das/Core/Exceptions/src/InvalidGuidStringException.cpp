#include <das/Core/Exceptions/InvalidGuidStringException.h>
#include <das/Utils/fmt.h>

DAS_CORE_EXCEPTIONS_NS_BEGIN

InvalidGuidStringException::InvalidGuidStringException(
    const std::string_view invalid_guid_string)
    : Base{DAS::fmt::format(
        "Invalid GUID string. Current value is {}.",
        invalid_guid_string)}
{
}

InvalidGuidStringSizeException::InvalidGuidStringSizeException(
    const std::size_t string_size)
    : Base{DAS::fmt::format(
        "Size of DasGuid string is not 36. Current value is {}.",
        string_size)}
{
}

DAS_CORE_EXCEPTIONS_NS_END