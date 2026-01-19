#include <das/Core/Exceptions/TypeError.h>
#include <das/Utils/fmt.h>
#include <magic_enum.hpp>

Das::Core::Exceptions::TypeError::TypeError(
    ExportInterface::DasType expected,
    ExportInterface::DasType actual)
    : Base{
          [](ExportInterface::DasType expected, ExportInterface::DasType actual)
          {
              const auto expected_int = magic_enum::enum_integer(expected);
              const auto actual_int = magic_enum::enum_integer(actual);
              const auto expected_string = magic_enum::enum_name(expected);
              const auto actual_string = magic_enum::enum_name(actual);
              return DAS::fmt::format(
                  "Unexpected type {}(value = {}) found. Expected type {}(value = {}).",
                  expected_string,
                  expected_int,
                  actual_string,
                  actual_int);
          }(expected, actual)}
{
}
