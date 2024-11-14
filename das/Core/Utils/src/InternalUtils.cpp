#include <das/Core/Utils/InternalUtils.h>

DAS_CORE_UTILS_NS_BEGIN

auto MakeDasReadOnlyStringFromUtf8(std::string_view u8_string)
    -> Das::Utils::Expected<DAS::DasPtr<IDasReadOnlyString>>
{
    IDasReadOnlyString* p_result{};
    const auto          error_code =
        ::CreateIDasReadOnlyStringFromUtf8(u8_string.data(), &p_result);

    if (IsOk(error_code))
    {
        DasPtr<IDasReadOnlyString> result{};
        *result.Put() = p_result;
        return result;
    }

    return DAS::Utils::MakeUnexpected(error_code);
}

DAS_CORE_UTILS_NS_END
