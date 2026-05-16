#pragma once

#include <das/Plugins/DasMaaPi/PiDto.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    struct PiRawMetadata
    {
        std::string              raw_json;
        std::vector<std::string> unknown_fields;
    };

    struct PiController
    {
        PiControllerDto          dto;
        std::vector<std::filesystem::path> resolved_attach_resource_paths;
        PiRawMetadata            raw;
    };

    struct PiResource
    {
        PiResourceDto                   dto;
        std::vector<std::filesystem::path> resolved_paths;
        PiRawMetadata                   raw;
    };

    struct PiTask
    {
        PiTaskDto     dto;
        std::string   raw_pipeline_override_json;
        PiRawMetadata raw;
    };

    struct PiGroup
    {
        PiGroupDto    dto;
        PiRawMetadata raw;
    };

    struct PiOption
    {
        PiOptionDto   dto;
        std::string   raw_pipeline_override_json;
        PiRawMetadata raw;
    };

    struct PiPreset
    {
        PiPresetDto   dto;
        PiRawMetadata raw;
    };

    struct PiCatalog
    {
        std::filesystem::path interface_path;
        std::filesystem::path interface_directory;
        std::string           name;
        std::string           version;
        std::map<std::string, std::filesystem::path> language_paths;
        std::map<std::string, std::map<std::string, std::string>> translations;
        std::vector<std::filesystem::path> imports;
        std::vector<PiController>          controllers;
        std::vector<PiResource>            resources;
        std::vector<PiTask>                tasks;
        std::vector<PiOption>              global_options;
        std::vector<PiOption>              options;
        std::vector<PiPreset>              presets;
        std::vector<PiGroup>               groups;
        std::vector<PiDiagnosticDto>       diagnostics;
        std::string                        raw_agent_json;
        PiRawMetadata                      raw;
    };

    const PiOption* FindOption(
        const PiCatalog& catalog,
        std::string_view name);
} // namespace Das::Plugins::DasMaaPi
