#ifndef ASR_CORE_FOREIGNINTERFACEHOST_ASRGUID_H
#define ASR_CORE_FOREIGNINTERFACEHOST_ASRGUID_H

#include <AutoStarRail/Core/Exceptions/InvalidGuidStringException.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/Config.h>
#include <AutoStarRail/IAsrBase.h>
#include <AutoStarRail/Utils/fmt.h>
#include <nlohmann/json_fwd.hpp>

template <>
struct std::hash<AsrGuid>
{
    std::size_t operator()(const AsrGuid& guid) const noexcept;
};

template <>
struct ASR_FMT_NS::formatter<AsrGuid, char>
    : public formatter<std::string, char>
{
    auto format(const AsrGuid& guid, format_context& ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator;
};

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

AsrGuid MakeAsrGuid(const std::string_view guid_string);

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

namespace nlohmann
{
    template <>
    struct adl_serializer<AsrGuid>
    {
        static void to_json(json& j, const AsrGuid& guid);

        static void from_json(const json& j, AsrGuid& guid);
    };
}

#endif // ASR_CORE_FOREIGNINTERFACEHOST_ASRGUID_H
