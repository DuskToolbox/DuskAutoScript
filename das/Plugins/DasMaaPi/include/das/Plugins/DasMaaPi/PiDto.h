#pragma once

#include <cassert>

#include <cpp_yyjson.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    struct PiDiagnosticDto
    {
        std::string                severity;
        std::string                code;
        std::string                message;
        std::optional<std::string> path;
        std::optional<std::string> source;
    };

    struct PiNamedDto
    {
        std::string                name;
        std::optional<std::string> label;
        std::optional<std::string> description;
        std::optional<std::string> icon;
    };

    struct PiControllerDto : PiNamedDto
    {
        std::string              type;
        std::vector<std::string> attach_resource_path;
        std::vector<std::string> option;
    };

    struct PiResourceDto : PiNamedDto
    {
        std::vector<std::string> path;
        std::vector<std::string> controller;
        std::vector<std::string> option;
        std::optional<std::string> hash;
    };

    struct PiTaskDto : PiNamedDto
    {
        std::string              entry;
        bool                     default_check = false;
        std::vector<std::string> controller;
        std::vector<std::string> resource;
        std::vector<std::string> group;
        std::vector<std::string> option;
    };

    struct PiGroupDto : PiNamedDto
    {
        bool default_expand = false;
    };

    struct PiCaseDto : PiNamedDto
    {
        std::vector<std::string> option;
    };

    struct PiInputDto : PiNamedDto
    {
        std::optional<std::string> default_value;
        std::optional<std::string> pipeline_type;
        std::optional<std::string> verify;
        std::optional<std::string> pattern_msg;
    };

    struct PiOptionDto
    {
        std::string                name;
        std::string                type;
        std::optional<std::string> label;
        std::optional<std::string> description;
        std::optional<std::string> icon;
        std::vector<std::string>   controller;
        std::vector<std::string>   resource;
        std::vector<PiCaseDto>     cases;
        std::vector<PiInputDto>    inputs;
        std::vector<std::string>   default_cases;
    };

    struct PiPresetTaskDto
    {
        std::string              name;
        bool                     enabled = true;
        std::vector<std::string> option_names;
    };

    struct PiPresetDto : PiNamedDto
    {
        std::vector<PiPresetTaskDto> task;
    };
} // namespace Das::Plugins::DasMaaPi

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiDiagnosticDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiNamedDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiControllerDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiResourceDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiTaskDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiGroupDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiCaseDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiInputDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiOptionDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiPresetTaskDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiPresetDto>
{
    using type = yyjson::snake_to_camel_transform;
};
