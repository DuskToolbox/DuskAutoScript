#pragma once

#include <cassert>

#include <cpp_yyjson.hpp>
#include <das/Utils/DasJsonCore.h>

#include <string>
#include <string_view>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    inline constexpr std::string_view kPluginGuidText =
        "69F20000-0000-4000-8000-000000000001";
    inline constexpr std::string_view kTaskGuidText =
        "69F20001-0000-4000-8000-000000000001";
    inline constexpr std::string_view kAuthoringFactoryGuidText =
        "69F20002-0000-4000-8000-000000000001";
    inline constexpr std::string_view kAgentComponentGuidText =
        "69F20004-0000-4000-8000-000000000001";
    inline constexpr std::string_view kAgentComponentFactoryGuidText =
        "69F20005-0000-4000-8000-000000000001";
    inline constexpr std::string_view kAgentTaskComponentGuidText =
        "69F20006-0000-4000-8000-000000000001";
    inline constexpr std::string_view kAgentTaskComponentFactoryGuidText =
        "69F20007-0000-4000-8000-000000000001";
    inline constexpr std::string_view kRunTaskComponentGuidText =
        "69F20008-0000-4000-8000-000000000001";
    inline constexpr std::string_view kRunTaskComponentFactoryGuidText =
        "69F20009-0000-4000-8000-000000000001";

    struct MaapiExecutionPolicyDto
    {
        bool fail_fast = true;
    };

    struct MaapiAdapterValuesDto
    {
        MaapiExecutionPolicyDto execution_policy{};
    };

    struct MaapiAuthoringValuesDto
    {
        MaapiAdapterValuesDto adapter{};
    };

    yyjson::value MakeAdapterOnlyDocument();
} // namespace Das::Plugins::DasMaaPi

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiExecutionPolicyDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiAdapterValuesDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiAuthoringValuesDto>
{
    using type = yyjson::snake_to_camel_transform;
};
