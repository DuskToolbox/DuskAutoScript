#ifndef DAS_CORE_FOREIGNINTERFACEHOST_DASGUID_H
#define DAS_CORE_FOREIGNINTERFACEHOST_DASGUID_H

#include <das/Core/Exceptions/InvalidGuidStringException.h>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/IDasBase.h>
#include <das/Utils/fmt.h>
#include <nlohmann/json_fwd.hpp>

template <>
struct std::hash<DasGuid>
{
    std::size_t operator()(const DasGuid& guid) const noexcept;
};

template <>
struct DAS_FMT_NS::formatter<DasGuid, char>
    : public formatter<std::string, char>
{
    auto format(const DasGuid& guid, format_context& ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DasGuid MakeDasGuid(const std::string_view guid_string);

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

namespace nlohmann
{
    template <>
    struct adl_serializer<DasGuid>
    {
        static void to_json(json& j, const DasGuid& guid);

        static void from_json(const json& j, DasGuid& guid);
    };
}

#endif // DAS_CORE_FOREIGNINTERFACEHOST_DASGUID_H
