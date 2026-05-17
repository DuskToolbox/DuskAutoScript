#pragma once

#include <cassert>

#include <cpp_yyjson.hpp>

#include <map>
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

    struct MaapiPiOptionSettingsDto
    {
        std::string                        option_name;
        std::string                        kind;
        std::vector<std::string>           selected_cases;
        std::map<std::string, std::string> input_values;
        std::optional<bool>                bool_value;
    };

    struct MaapiPiTaskSettingsDto
    {
        std::string                           task_name;
        bool                                  enabled = true;
        std::vector<MaapiPiOptionSettingsDto> options;
    };

    struct MaapiPiSettingsDto
    {
        std::optional<std::string>            controller_name;
        std::optional<std::string>            resource_name;
        std::optional<std::string>            preset_name;
        std::vector<MaapiPiOptionSettingsDto> global_options;
        std::vector<MaapiPiOptionSettingsDto> resource_options;
        std::vector<MaapiPiOptionSettingsDto> controller_options;
        std::vector<MaapiPiTaskSettingsDto>   tasks;
        std::vector<std::string>              orphan_paths;
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
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiPiOptionSettingsDto>
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
