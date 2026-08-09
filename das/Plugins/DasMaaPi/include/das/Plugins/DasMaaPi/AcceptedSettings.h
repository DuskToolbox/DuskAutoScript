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
        std::optional<std::string>      proxy_url;
        std::optional<std::string>      mirrorchyan_cdk;
        std::optional<std::string>      update_channel;
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

    // D-86-03/06: pretask 选中 option 值的 Provider 私有存储。stable
    // pretask_id 遵循 D-86-03（dto.name 非空→name，否则 exec）。镜像
    // custom_presets 的私有存储模式：不进入 document UI IR 的 id 层，只存
    // option 选中值用于 Get 投影与 setValue 回写。
    struct MaapiPiPretaskSettingsDto
    {
        std::string                           pretask_id;
        std::vector<MaapiPiOptionSettingsDto> options;
    };

    // 用户配置实例快照 (D-84.2-04)。
    // D-84.2-01 已删除 configurations[] 多配置变体模型；customPresets 取代
    // 旧的 configuration 概念作为 Provider 私有 preset 存储（见下文）。

    // PI 权威设置层。
    // flat 字段 (controller_name / resource_name / language /
    // global_options / resource_options / controller_options / tasks) 是
    // acceptedProperties.pi 的当前权威；preset_name 是当前选中的 preset 引用
    // ——可以是 PI 官方 preset 模板（只读，D-84-42），也可以是 custom_presets
    // 中的用户快照（newPreset 创建后自动选中，D-84.2-04）。
    // orphan_paths 保留在 pi 层，是 Provider 私有诊断路径，不进入 UI IR
    // (D-84-04)。
    struct MaapiPiSettingsDto
    {
        std::optional<std::string> controller_name;
        std::optional<std::string> resource_name;
        std::optional<std::string> preset_name;
        std::optional<std::string> language;
        // 选中 ADB 设备的 address key。value 是 AdbDeviceInfo.address
        // （Toolkit 探测到的稳定设备身份）。完整六字段
        // （adb_path / address / screencap_methods / input_methods / config）
        // 运行时从 session device_cache_ 取 Toolkit
        // 推荐值，不持久化（agent_path 仍走 PI
        // raw_json）。空表示用户未选，编译期 fallback raw_json。
        std::optional<std::string>            controller_adb_device;
        std::vector<MaapiPiOptionSettingsDto> global_options;
        std::vector<MaapiPiOptionSettingsDto> resource_options;
        std::vector<MaapiPiOptionSettingsDto> controller_options;
        std::vector<MaapiPiTaskSettingsDto>   tasks;

        // D-86-03/06: pretask 选中值私有存储。pretask_id 遵循
        // name||exec（D-86-03）；不进入 document UI IR 的 id 层，只供
        // pretask.<key> 根字段读写。
        std::vector<MaapiPiPretaskSettingsDto> pretasks;

        // D-84.2-04: Provider 私有 preset 快照存储。newPreset invokeCommand
        // 追加当前 flat pi 字段的镜像到此数组；投影层把 id/name 合并到
        // presetName select candidates。customPresets 不进入 document UI IR。
        std::vector<MaapiPresetSnapshotDto> custom_presets;

        std::vector<std::string> orphan_paths;

        // schedule 配置。flat optional 字段（非子 DTO），规避 cpp_yyjson
        // optional<聚合体首字段 string> caster 陷阱。PiCompiler 组装成
        // envelope.maapi.schedule。缺失 = nullopt = schedule off（kind=""）。
        // interval_minutes / daily_catch_up 为预留字段。optional<bool> caster
        // 已 在 ExecutionEnvelope.h 显式特化，沿用。
        std::optional<std::string> schedule_kind;
        std::optional<int>         schedule_interval_minutes;
        std::optional<std::string> schedule_daily_time;
        std::optional<bool>        schedule_daily_catch_up;
    };

    // 通用 UI 状态 kv 存储（视图偏好 + 插件自定义数据）。
    // key → 任意 JSON value；经 Apply uiEvent kind 写入（ApplyUiEventChange
    // handler 按 event type 映射到 key，如 sectionExpandChanged →
    // "sectionExpand.<id>"）。 不参与 PI 业务语义/编译，纯存储；Get 时前端读
    // values 自行合成 UI 状态 （后端不耦合 sectionExpand 等 UI 概念）。
    struct MaapiUiSettingsDto
    {
        std::map<std::string, yyjson::value> values;
    };

    struct AcceptedSettingsDto
    {
        int32_t                 version = 1;
        MaapiAdapterSettingsDto adapter;
        MaapiPiSettingsDto      pi;
        MaapiUiSettingsDto      ui;
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
struct yyjson::field_name_rule<
    Das::Plugins::DasMaaPi::MaapiPiPretaskSettingsDto>
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
