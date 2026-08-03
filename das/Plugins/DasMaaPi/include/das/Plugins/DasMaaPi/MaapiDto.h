#pragma once

#include <cassert>

#include <cpp_yyjson.hpp>
#include <das/Utils/DasJsonCore.h>

#include <string>
#include <string_view>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    inline constexpr std::string_view PLUGIN_GUID_TEXT =
        "69F20000-0000-4000-8000-000000000001";
    inline constexpr std::string_view TASK_GUID_TEXT =
        "69F20001-0000-4000-8000-000000000001";
    // 与 manifest DasMaaPi.json:version 对齐，作为 PI client_version 注入
    // envelope（D-86-12）。manifest 与此常量须保持同步。
    inline constexpr std::string_view PLUGIN_VERSION_TEXT = "0.1";
    inline constexpr std::string_view AUTHORING_FACTORY_GUID_TEXT =
        "69F20002-0000-4000-8000-000000000001";
    inline constexpr std::string_view AGENT_COMPONENT_GUID_TEXT =
        "69F20004-0000-4000-8000-000000000001";
    inline constexpr std::string_view AGENT_COMPONENT_FACTORY_GUID_TEXT =
        "69F20005-0000-4000-8000-000000000001";
    inline constexpr std::string_view AGENT_TASK_COMPONENT_GUID_TEXT =
        "69F20006-0000-4000-8000-000000000001";
    inline constexpr std::string_view AGENT_TASK_COMPONENT_FACTORY_GUID_TEXT =
        "69F20007-0000-4000-8000-000000000001";
    inline constexpr std::string_view RUN_TASK_COMPONENT_GUID_TEXT =
        "69F20008-0000-4000-8000-000000000001";
    inline constexpr std::string_view RUN_TASK_COMPONENT_FACTORY_GUID_TEXT =
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
