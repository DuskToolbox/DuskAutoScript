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

    // 每个已添加的 PI task 实例的持久化设置。
    // item_id 是 Provider 颁发的稳定实例身份，跨 Get/Apply/Compile 保持，
    // 不从 taskName/actionId/index/label 或随机 Get-time 值派生
    // (D-84-40)。同一 PI task 类型允许被添加多次，故 task_name 仅表示
    // 业务类型，不能充当实例身份。
    struct MaapiPiTaskSettingsDto
    {
        std::string                           item_id;
        std::string                           task_name;
        bool                                  enabled = true;
        std::vector<MaapiPiOptionSettingsDto> options;
    };

    // D-84.2-04: Provider 私有 preset 快照。newPreset invokeCommand 把当前
    // flat pi 字段镜像为一个 snapshot，追加到 pi.custom_presets。这是 MaaPI
    // 私有存储，不进入 document UI IR（FormSequenceItem.id / FieldDescriptor
    // 不读 customPresets）；投影层只把 id/name 合并到 presetName select
    // candidates 作为 {value,label}。
    struct MaapiPresetSnapshotDto
    {
        std::string                           id;
        std::string                           name;
        std::optional<std::string>            controller_name;
        std::optional<std::string>            resource_name;
        std::optional<std::string>            language;
        std::vector<MaapiPiOptionSettingsDto> global_options;
        std::vector<MaapiPiOptionSettingsDto> resource_options;
        std::vector<MaapiPiOptionSettingsDto> controller_options;
        std::vector<MaapiPiTaskSettingsDto>   tasks;
    };

    // 用户配置实例快照 (D-84-41/42)。
    // 每个 configuration 独立持有 controller/resource/language/options/tasks，
    // 是 acceptedProperties 中完整的可编辑权威。PI 官方 preset 是只读模板，
    // 不修改 interface.json，也不充当 configuration 身份。
    struct MaapiConfigurationDto
    {
        std::string                           id;
        std::string                           name;
        std::optional<std::string>            controller_name;
        std::optional<std::string>            resource_name;
        std::optional<std::string>            language;
        std::vector<MaapiPiOptionSettingsDto> global_options;
        std::vector<MaapiPiOptionSettingsDto> resource_options;
        std::vector<MaapiPiOptionSettingsDto> controller_options;
        std::vector<MaapiPiTaskSettingsDto>   tasks;
    };

    // PI 权威设置层。
    // configurations[] 是配置实例的 source of truth (post-84)；
    // selected_configuration_id 指向当前生效的 configuration。
    // preset_name 是 PI 官方 preset 模板引用，只读 (D-84-42)。
    // orphan_paths 保留在 pi 层，是 Provider 私有诊断路径，不进入 UI IR
    // (D-84-04)。
    // legacy flat fields (controller_name / resource_name / language /
    // global_options / resource_options / controller_options / tasks) 保留
    // 作为前 84 时代的隐式单配置 fallback：parser 读到无 configurations[] 的
    // legacy 输入时填入 flat 字段并 emit 一条 diagnostic 警告，不做身份迁移
    // (Phase 84.1 owns reconstruction)。ProjectAuthoringDocument 在
    // configurations 为空时回退到 flat 字段，从而保护既有未迁移 fixture。
    struct MaapiPiSettingsDto
    {
        std::optional<std::string>            controller_name;
        std::optional<std::string>            resource_name;
        std::optional<std::string>            preset_name;
        std::optional<std::string>            language;
        std::vector<MaapiPiOptionSettingsDto> global_options;
        std::vector<MaapiPiOptionSettingsDto> resource_options;
        std::vector<MaapiPiOptionSettingsDto> controller_options;
        std::vector<MaapiPiTaskSettingsDto>   tasks;

        std::string                        selected_configuration_id;
        std::vector<MaapiConfigurationDto> configurations;

        // D-84.2-04: Provider 私有 preset 快照存储。newPreset invokeCommand
        // 追加当前 flat pi 字段的镜像到此数组；投影层把 id/name 合并到
        // presetName select candidates。customPresets 不进入 document UI IR。
        std::vector<MaapiPresetSnapshotDto> custom_presets;

        std::vector<std::string> orphan_paths;
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
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiPresetSnapshotDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::MaapiConfigurationDto>
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
