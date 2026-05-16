#pragma once

#include <cassert>

#include <cpp_yyjson.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    struct MaapiExecutionPolicySettingsDto
    {
        bool fail_fast = true;
    };

    struct MaapiAdapterSettingsDto
    {
        std::optional<std::string>      interface_path;
        std::optional<std::string>      project_root;
        std::optional<std::string>      source_fingerprint;
        MaapiExecutionPolicySettingsDto execution_policy;
    };

    struct MaapiPiTaskSettingsDto
    {
        std::string task_name;
        bool        enabled = true;
    };

    struct MaapiPiSettingsDto
    {
        std::optional<std::string>       controller_name;
        std::optional<std::string>       resource_name;
        std::optional<std::string>       preset_name;
        std::vector<MaapiPiTaskSettingsDto> tasks;
        std::vector<std::string>         orphan_paths;
    };

    struct AcceptedSettingsDto
    {
        int32_t                 version = 1;
        MaapiAdapterSettingsDto adapter;
        MaapiPiSettingsDto      pi;
    };
} // namespace Das::Plugins::DasMaaPi

template <>
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::MaapiExecutionPolicySettingsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiAdapterSettingsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiPiTaskSettingsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiPiSettingsDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::AcceptedSettingsDto>
{
    using type = yyjson::snake_to_camel_transform;
};
