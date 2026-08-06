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
    // DasMaaPi 作为 PI Client 实现的 PI 扩展能力协议版本（semver，见
    // build/mingw-debug/_deps/maaframework_binary-src/docs/zh_cn/
    // 3.3-ProjectInterfaceV2协议.md:451 「PI_INTERFACE_VERSION」）。与
    // interface.json 顶层 interface_version（固定数字 2，JSON 结构主版本）
    // 是两个不同概念，勿混淆。当前对齐 v2.8.1（pretask controller/resource
    // filter）；当 DasMaaPi 增量支持 v2.9.0+ 特性时手动 bump 并核查所有
    // agent 子进程消费方。
    inline constexpr std::string_view PI_EXTENSION_VERSION_TEXT = "v2.8.1";
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
