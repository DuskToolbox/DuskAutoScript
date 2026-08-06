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
        // D-84-11: 严格 diagnostic 允许可选整数 DasResult
        // code。PiDiagnosticDto.code 是 PI 协议遗留的字符串形态（不在 84-01
        // 严格 wire 中输出）； int_code 是 Phase 84 Authoring envelope 的整数
        // code。
        std::optional<std::int64_t> int_code;
    };

    struct PiNamedDto
    {
        std::string                name;
        std::optional<std::string> label;
        std::optional<std::string> description;
        std::optional<std::string> icon;
    };

    // PI v2 controller typed config blocks. Strings stay as protocol text;
    // the compiler layer translates them to MaaFW enum bitmasks via
    // ControllerMethodMaps. Keeping them as strings in the DTO lets the parser
    // stay byte-faithful (raw_json + typed block) while downstream consumers
    // (NormalizeControllerInitParams) own the string→enum policy.
    struct Win32ConfigDto
    {
        std::string class_regex;
        std::string window_regex;
        std::string screencap; // single method, predefined combo, or '|'-OR set
        std::string mouse;     // single-select MaaWin32InputMethod name
        std::string keyboard;  // single-select MaaWin32InputMethod name
    };

    struct GamepadConfigDto
    {
        std::string class_regex;
        std::string window_regex;
        std::string gamepad_type; // Xbox360 / DualShock4 / DS4 alias
        std::string screencap;    // single method or combo (hWnd may be null)
    };

    struct PiControllerDto : PiNamedDto
    {
        std::string                     type;
        std::vector<std::string>        attach_resource_path;
        std::vector<std::string>        option;
        std::optional<Win32ConfigDto>   win32;
        std::optional<GamepadConfigDto> gamepad;
    };

    struct PiResourceDto : PiNamedDto
    {
        std::vector<std::string>   path;
        std::vector<std::string>   controller;
        std::vector<std::string>   option;
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
        bool default_expand = true;
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
        std::string                          name;
        bool                                 enabled = true;
        std::vector<std::string>             option_names;
        std::map<std::string, yyjson::value> option_values;
    };

    struct PiPresetDto : PiNamedDto
    {
        std::vector<PiPresetTaskDto> task;
    };

    // PI v2.7.0 — 顶层 pretask（Controller 启动前按序执行的外部进程）。
    // exec 是唯一必填字段；name/label/description/icon 走 PiNamedDto；
    // option/controller/resource 为 v2.8.1 激活过滤用键列表。
    struct PiPretaskDto : PiNamedDto
    {
        std::string              exec;
        std::vector<std::string> args;
        std::vector<std::string> option;
        std::vector<std::string> controller;
        std::vector<std::string> resource;
    };

    // PI v2.8.0 — 任务设置页 UI 声明分区，option 引用 option 块中已定义的键。
    // default_expand 协议默认为 true（PI v2 group v2.4.0 / setting v2.8.0
    // 一致，参见 3.3-ProjectInterfaceV2协议.md:494 与 :802）。
    struct PiSettingDto : PiNamedDto
    {
        std::vector<std::string> option;
        bool                     default_expand = true;
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
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::Win32ConfigDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::GamepadConfigDto>
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

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiPretaskDto>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<Das::Plugins::DasMaaPi::PiSettingDto>
{
    using type = yyjson::snake_to_camel_transform;
};
