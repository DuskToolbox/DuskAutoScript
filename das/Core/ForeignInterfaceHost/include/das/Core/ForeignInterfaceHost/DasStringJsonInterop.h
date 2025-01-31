//
// Created by Dusk on 25-1-31.
//

#ifndef DAS_CORE_FOREIGNINTERFACEHOST_DASSTRINGJSONINTEROP_H
#define DAS_CORE_FOREIGNINTERFACEHOST_DASSTRINGJSONINTEROP_H

#include <das/DasString.hpp>
#include <nlohmann/json_fwd.hpp>

namespace nlohmann
{
    template <>
    struct DAS_API adl_serializer<DasReadOnlyString>
    {
        static void to_json(json& j, const DasReadOnlyString& das_string);

        static void from_json(const json& j, DasReadOnlyString& das_string);
    };

    template <>
    struct DAS_API adl_serializer<DAS::DasPtr<IDasReadOnlyString>>
    {
        static void to_json(
            json&                                  j,
            const DAS::DasPtr<IDasReadOnlyString>& p_das_string);

        static void from_json(
            const json&                      j,
            DAS::DasPtr<IDasReadOnlyString>& p_das_string);
    };
}

#endif // DAS_CORE_FOREIGNINTERFACEHOST_DASSTRINGJSONINTEROP_H
