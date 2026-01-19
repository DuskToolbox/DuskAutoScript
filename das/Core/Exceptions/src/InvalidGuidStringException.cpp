#include <das/Core/Exceptions/InvalidGuidStringException.h>
#include <das/Utils/fmt.h>

DAS_CORE_EXCEPTIONS_NS_BEGIN

InvalidGuidStringException::InvalidGuidStringException(
    const std::string_view invalid_guid_string)
    : DasException{
          DAS_E_INVALID_STRING,
          DAS_FMT_NS::format(
              "Invalid GUID string. Current value is {}.",
              invalid_guid_string)}
{
}

InvalidGuidStringSizeException::InvalidGuidStringSizeException(
    const std::size_t string_size)
    : DasException{
          DAS_E_INVALID_STRING_SIZE,
          DAS::fmt::format(
              "Size of DasGuid string is not 36. Current value is {}.",
              string_size)}
{
}

DAS_CORE_EXCEPTIONS_NS_END