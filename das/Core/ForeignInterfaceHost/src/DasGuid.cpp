#include "das/DasString.hpp"
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <array>
#include <bit>
#include <boost/container_hash/hash.hpp>
#include <cstdio>
#include <cstring>
#include <functional>
#include <nlohmann/json.hpp>

std::size_t std::hash<DasGuid>::operator()(const DasGuid& guid) const noexcept
{
    using _internal_asr_Guid = std::array<int64_t, 2>;

    /**
     * @brief std::bit_cast require GCC 11 or Clang 14 or MSVC 19.27
     *
     */
    const auto guid_data = std::bit_cast<_internal_asr_Guid>(guid);
    return boost::hash_range(guid_data.cbegin(), guid_data.cend());
}

auto(DAS_FMT_NS::formatter<DasGuid, char>::format)(
    const DasGuid&  guid,
    format_context& ctx) const ->
    typename std::remove_reference_t<decltype(ctx)>::iterator
{
    constexpr auto template_string =
        "{{{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}}}";

    return formatter<std::string>::format(
        DAS::fmt::format(
            template_string,
            guid.data1,
            guid.data2,
            guid.data3,
            guid.data4[0],
            guid.data4[1],
            guid.data4[2],
            guid.data4[3],
            guid.data4[4],
            guid.data4[5],
            guid.data4[6],
            guid.data4[7]),
        ctx);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DasGuid MakeDasGuid(const std::string_view guid_string)
{
    DasGuid      result;
    unsigned int p0;
    static_assert(sizeof(p0) == sizeof(result.data1), "p0 type not match.");
    unsigned short p1, p2;
    static_assert(sizeof(p1) == sizeof(result.data2), "p1 type not match.");
    unsigned char p3, p4, p5, p6, p7, p8, p9, p10;
    static_assert(sizeof(p3) == sizeof(result.data4[0]), "p3 type not match.");
    std::array<char, 37> tmp_buffer;

    // 检查GUID字符串长度
    if (const auto string_size = guid_string.size(); //
        string_size != 36)
    {
        throw InvalidGuidStringSizeException(string_size);
    }

    std::memcpy(tmp_buffer.data(), guid_string.data(), 36);
    tmp_buffer[36] = '\0';

#ifdef _MSC_VER
#define DAS_SSCANF ::sscanf_s
#else
#define DAS_SSCANF std::sscanf
#endif // _MSC_VER

    if (const auto err = DAS_SSCANF(
            guid_string.data(),
            "%08X-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
            &p0,
            &p1,
            &p2,
            &p3,
            &p4,
            &p5,
            &p6,
            &p7,
            &p8,
            &p9,
            &p10);
        err != 11)
    {
        throw InvalidGuidStringException(guid_string);
    }

    result.data1 = p0;
    result.data2 = p1;
    result.data3 = p2;
    result.data4[0] = p3;
    result.data4[1] = p4;
    result.data4[2] = p5;
    result.data4[3] = p6;
    result.data4[4] = p7;
    result.data4[5] = p8;
    result.data4[6] = p9;
    result.data4[7] = p10;

    return result;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

void nlohmann::adl_serializer<DasGuid>::to_json(json& j, const DasGuid& guid)
{
    const auto guid_string = DAS::fmt::format("{}", guid);
    j = guid_string;
}

void nlohmann::adl_serializer<DasGuid>::from_json(const json& j, DasGuid& guid)
{
    const auto guid_string = j.get<std::string_view>();
    guid = DAS::Core::ForeignInterfaceHost::MakeDasGuid(guid_string);
}

DasReadOnlyString DasGuidToString(const DasGuid& guid)
{
    constexpr auto template_string =
        "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}";
    const auto string = DAS::fmt::format(
        template_string,
        guid.data1,
        guid.data2,
        guid.data3,
        guid.data4[0],
        guid.data4[1],
        guid.data4[2],
        guid.data4[3],
        guid.data4[4],
        guid.data4[5],
        guid.data4[6],
        guid.data4[7]);

    DAS::DasPtr<IDasString> p_result;
    ::CreateIDasStringFromUtf8(string.c_str(), p_result.Put());
    return DasReadOnlyString{p_result};
}