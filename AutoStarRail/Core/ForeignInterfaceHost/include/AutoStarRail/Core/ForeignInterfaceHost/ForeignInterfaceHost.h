#ifndef ASR_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOST_H
#define ASR_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOST_H

#include <AutoStarRail/Core/ForeignInterfaceHost/Config.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/ForeignInterfaceHostEnum.h>
#include <AutoStarRail/ExportInterface/AsrJson.h>
#include <AutoStarRail/ExportInterface/IAsrSettings.h>
#include <AutoStarRail/IAsrBase.h>
#include <AutoStarRail/Utils/fmt.h>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>


ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

struct PluginSettingDesc;
struct PluginDesc;

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

template <>
struct ASR_FMT_NS::
    formatter<ASR::Core::ForeignInterfaceHost::PluginSettingDesc, char>
    : public formatter<std::string, char>
{
    auto format(
        const ASR::Core::ForeignInterfaceHost::PluginSettingDesc& desc,
        format_context&                                           ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator;
};

template <>
struct ASR_FMT_NS::formatter<ASR::Core::ForeignInterfaceHost::PluginDesc, char>
    : public formatter<std::string, char>
{
    auto format(
        const ASR::Core::ForeignInterfaceHost::PluginDesc& desc,
        format_context&                                    ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator;
};

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

/**
 * @brief 当更新这一结构体时，记得更新相关formatter
 *  （位于 AutoStarRail/Core/ForeignInterfaceHost/src/ForeignInterfaceHost.cpp）
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

    AsrType type = ASR_TYPE_STRING;
    /**
     * @brief 保留参数，不参与序列化
     *
     */
    // AsrSettingScope scope = AsrSettingScope::Global;
};

void from_json(const ::nlohmann::json& input, PluginSettingDesc& output);
// void to_json(const ::nlohmann::json& output, PluginSettingDesc& input);

struct PluginDesc
{
    ForeignInterfaceLanguage       language;
    std::string                    name;
    std::string                    description;
    std::string                    author;
    std::string                    version;
    std::string                    supported_system;
    std::string                    plugin_filename_extension;
    std::optional<std::string>     opt_resource_path;
    AsrGuid                        guid;
    std::vector<PluginSettingDesc> settings;
};

void from_json(const ::nlohmann::json& input, PluginDesc& output);
// void to_json(const ::nlohmann::json& output, PluginDesc& input);

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // ASR_CORE_FOREIGNINTERFACEHOST_FOREIGNINTERFACEHOST_H