#ifndef DAS_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOST_H
#define DAS_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOST_H

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHostEnum.h>
#include <das/IDasBase.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/DasJson.h>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <boost/signals2/signal.hpp>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

struct PluginSettingDesc;
struct PluginPackageDesc;

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

template <>
struct DAS_FMT_NS::
    formatter<DAS::Core::ForeignInterfaceHost::PluginSettingDesc, char>
    : public formatter<std::string, char>
{
    auto format(
        const DAS::Core::ForeignInterfaceHost::PluginSettingDesc& desc,
        format_context&                                           ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator;
};

template <>
struct DAS_FMT_NS::
    formatter<DAS::Core::ForeignInterfaceHost::PluginPackageDesc, char>
    : public formatter<std::string, char>
{
    auto format(
        const DAS::Core::ForeignInterfaceHost::PluginPackageDesc& desc,
        format_context&                                           ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

/**
 * @brief 当更新这一结构体时，记得更新相关formatter
 *  （位于 das/Core/ForeignInterfaceHost/src/ForeignInterfaceHost.cpp）
 */
struct PluginSettingDesc
{
    std::string name;
    std::variant<std::monostate, bool, std::int64_t, float, std::string>
                                            default_value;
    std::optional<std::string>              description;
    std::optional<std::vector<std::string>> enum_values;
    std::optional<std::vector<std::string>> enum_descriptions;
    std::optional<std::string>              deprecation_message;
    bool                                    required = false;

    ExportInterface::DasType type = Das::ExportInterface::DAS_TYPE_STRING;
    /**
     * @brief 保留参数，不参与序列化
     *
     */
    // DasSettingScope scope = DasSettingScope::Global;
};

void ParsePluginSettingDescFromJson(
    const yyjson::writer::const_object_ref& input,
    PluginSettingDesc&                      output);

/**
 * @brief Plugin-GUID-keyed settings descriptor group.
 * Each plugin GUID maps to a group with a name, description, and descriptor
 * list.
 */
struct PluginSettingsGroup
{
    std::string                    name;
    std::string                    description;
    std::vector<PluginSettingDesc> descriptors;
};

void ParsePluginSettingsGroupFromJson(
    const yyjson::writer::const_object_ref&           input,
    std::unordered_map<DasGuid, PluginSettingsGroup>& output);

/// Declarative authoring capability attached to a task descriptor.
struct TaskAuthoringCapabilityDesc
{
    DasGuid                  factory_guid{};
    std::vector<std::string> supported_kinds;
};

/// Inline task component definition routed by a factory implementation GUID.
struct TaskComponentManifestEntryDesc
{
    std::optional<std::string> factory_guid;
    std::optional<yyjson::value> definition;
};

/// Top-level taskComponents manifest section.
struct TaskComponentsManifestDesc
{
    std::optional<std::vector<std::string>> factories;
    std::optional<
        std::unordered_map<std::string, TaskComponentManifestEntryDesc>>
        components;
};

struct TaskComponentsValidationResult
{
    std::vector<std::string> rejection_reasons;

    [[nodiscard]] bool IsValid() const
    {
        return rejection_reasons.empty();
    }
};

TaskComponentsValidationResult ValidateTaskComponents(
    const TaskComponentsManifestDesc& task_components);

/// Declarative task-component capability attached to a task descriptor.
struct TaskComponentCapabilityDesc
{
    DasGuid     component_guid;
    uint64_t    factory_feature_index = 0;
    std::string role;
};

/**
 * @brief Task type descriptor keyed by task GUID in the manifest.
 * Declares task metadata and property descriptors for scheduler task instances.
 */
struct TaskDescriptor
{
    DasGuid                                    plugin_guid;
    std::string                                name;
    std::string                                description;
    std::optional<std::string>                 game_name;
    std::vector<PluginSettingDesc>             descriptors;
    std::optional<TaskAuthoringCapabilityDesc> authoring;
    std::vector<TaskComponentCapabilityDesc>   components;
};

void ParseTaskDescriptorFromJson(
    const yyjson::writer::const_object_ref& input,
    TaskDescriptor&                         output);

struct PluginPackageDesc
{
    ForeignInterfaceLanguage       language;
    LoadMode                       load_mode = LoadMode::InProcess;
    std::string                    name;
    std::string                    description;
    std::string                    author;
    std::string                    version;
    std::string                    supported_system;
    std::string                    plugin_filename_extension;
    std::optional<std::string>     opt_resource_path;
    DasGuid                        guid;
    std::vector<PluginSettingDesc> settings_desc;

    /// Plugin-GUID-keyed settings descriptor groups from manifest.
    std::unordered_map<DasGuid, PluginSettingsGroup> settings_groups;
    /// Task-GUID-keyed task descriptors from manifest.
    std::unordered_map<DasGuid, TaskDescriptor> task_descriptors;
    /// Top-level taskComponents manifest data for component factory routing.
    std::optional<TaskComponentsManifestDesc> task_components;

    class SettingsJson
    {
    public:
        void SetValue(IDasReadOnlyString* p_json);
        void GetValue(IDasReadOnlyString** pp_out_json);

    private:
        std::mutex                 mutex_{};
        DasPtr<IDasReadOnlyString> settings_json_{};
    };
    // 下面的变量不被序列化到json
    std::shared_ptr<SettingsJson> settings_json_ =
        std::make_shared<SettingsJson>();
    DasReadOnlyStringWrapper settings_desc_json;
    yyjson::value            default_settings;
    boost::signals2::signal<void(std::shared_ptr<SettingsJson>)>
        on_settings_changed{};
};

void ParsePluginPackageDescFromJson(
    const yyjson::writer::const_object_ref& input,
    PluginPackageDesc&                      output);

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

template <>
struct yyjson::field_name_rule<
    DAS::Core::ForeignInterfaceHost::TaskAuthoringCapabilityDesc>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    DAS::Core::ForeignInterfaceHost::TaskComponentManifestEntryDesc>
{
    using type = yyjson::snake_to_camel_transform;
};

template <>
struct yyjson::field_name_rule<
    DAS::Core::ForeignInterfaceHost::TaskComponentsManifestDesc>
{
    using type = yyjson::snake_to_camel_transform;
};

#endif // DAS_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOST_H
