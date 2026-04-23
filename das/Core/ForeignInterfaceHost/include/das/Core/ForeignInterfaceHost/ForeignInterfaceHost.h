#ifndef DAS_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOST_H
#define DAS_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOST_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHostEnum.h>
#include <das/IDasBase.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/DasJson.h>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <unordered_map>
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

void from_json(const ::nlohmann::json& input, PluginSettingDesc& output);
// void to_json(const ::nlohmann::json& output, PluginSettingDesc& input);

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

void from_json(
    const ::nlohmann::json&                           input,
    std::unordered_map<DasGuid, PluginSettingsGroup>& output);

/**
 * @brief Task type descriptor keyed by task GUID in the manifest.
 * Declares task metadata and property descriptors for scheduler task instances.
 */
struct TaskDescriptor
{
    DasGuid                        plugin_guid;
    std::string                    name;
    std::string                    description;
    std::optional<std::string>     game_name;
    std::vector<PluginSettingDesc> descriptors;
};

void from_json(const ::nlohmann::json& input, TaskDescriptor& output);

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
    nlohmann::json           default_settings;
    boost::signals2::signal<void(std::shared_ptr<SettingsJson>)>
        on_settings_changed{};
};

void from_json(const ::nlohmann::json& input, PluginPackageDesc& output);
// void to_json(const ::nlohmann::json& output, PluginDesc& input);

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOST_H