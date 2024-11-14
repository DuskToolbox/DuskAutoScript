#include <das/DasConfig.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/Logger/Logger.h>
#include <das/ExportInterface/IDasSettings.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/EnumUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/UnexpectedEnumException.h>
#include <magic_enum_format.hpp>
#include <stdexcept>

// Compatible with fmt
#if DAS_USE_STD_FMT

template <class T>
struct DAS_FMT_NS::formatter<std::vector<T>, char>
    : public formatter<std::string, char>
{
    auto format(const std::vector<T>& value, format_context& ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator
    {
        auto result = DAS_FMT_NS::format_to(ctx.out(), "[");
        std::for_each(
            value.begin(),
            std::prev(value.end()),
            [&result](const auto& s)
            { result = DAS_FMT_NS::format_to(result, "{},", s); });
        if (!value.empty() && value.begin() != value.end())
        {
            result = DAS_FMT_NS::format_to(result, "{}", value.back());
        }
        result = DAS_FMT_NS::format_to(result, "]");
        return result;
    }
};

#endif

template <class T>
struct DAS_FMT_NS::formatter<std::optional<std::vector<T>>, char>
    : public formatter<std::string, char>
{
    auto format(
        const std::optional<std::vector<T>>& opt_value,
        format_context&                      ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator
    {
        auto result = ctx.out();
        if (opt_value)
        {
            result = DAS_FMT_NS::format_to(result, "{}", opt_value.value());
        }
        else
        {
            result = DAS_FMT_NS::format_to(result, "null");
        }
        return result;
    }
};

template <class T>
struct DAS_FMT_NS::formatter<std::optional<T>, char>
    : public formatter<std::string, char>
{
    auto format(const std::optional<T>& opt_value, format_context& ctx) const ->
        typename std::remove_reference_t<decltype(ctx)>::iterator
    {
        auto result = ctx.out();
        if (opt_value)
        {
            DAS_FMT_NS::format_to(result, "{}", opt_value.value());
        }
        else
        {
            DAS_FMT_NS::format_to(result, "null");
        }
        return result;
    }
};

#define DAS_CORE_FOREIGNINTERFACEHOST_VERIFY_AND_FORMAT(enum_value, type)      \
    case enum_value:                                                           \
    {                                                                          \
        auto* const p_value = std::get_if<type>(&desc.default_value);          \
        if (p_value == nullptr)                                                \
        {                                                                      \
            result = DAS_FMT_NS::format_to(                                    \
                result,                                                        \
                "Unexpected value. Expected type is \"" #type "\".");          \
            break;                                                             \
        }                                                                      \
        result =                                                               \
            DAS_FMT_NS::format_to(result, "default_value = {}\n", *p_value);   \
        break;                                                                 \
    }

#define DAS_CORE_FOREIGNINTERFACEHOST_VAR(x) #x " = {}\n", desc.x

auto(DAS_FMT_NS::formatter<
     DAS::Core::ForeignInterfaceHost::PluginSettingDesc,
     char>::format)(
    const DAS::Core::ForeignInterfaceHost::PluginSettingDesc& desc,
    format_context&                                           ctx) const ->
    typename std::remove_reference_t<decltype(ctx)>::iterator
{
    auto result = ctx.out();
    // 当PluginSettingDesc更新时记得更新这里
    result =
        DAS_FMT_NS::format_to(result, DAS_CORE_FOREIGNINTERFACEHOST_VAR(name));
    if (std::get_if<std::monostate>(&desc.default_value) == nullptr)
    {
        switch (desc.type)
        {
            DAS_CORE_FOREIGNINTERFACEHOST_VERIFY_AND_FORMAT(
                DAS_TYPE_BOOL,
                bool);
            DAS_CORE_FOREIGNINTERFACEHOST_VERIFY_AND_FORMAT(
                DAS_TYPE_INT,
                std::int64_t);
            DAS_CORE_FOREIGNINTERFACEHOST_VERIFY_AND_FORMAT(
                DAS_TYPE_FLOAT,
                float);
            DAS_CORE_FOREIGNINTERFACEHOST_VERIFY_AND_FORMAT(
                DAS_TYPE_STRING,
                std::string);
        default:
            throw DAS::Utils::UnexpectedEnumException::FromEnum(desc.type);
        }
    }
    else
    {
        result = DAS_FMT_NS::format_to(result, "empty default value\n");
    }
    result = DAS_FMT_NS::format_to(
        result,
        DAS_CORE_FOREIGNINTERFACEHOST_VAR(description));
    result = DAS_FMT_NS::format_to(
        result,
        DAS_CORE_FOREIGNINTERFACEHOST_VAR(enum_values));
    result = DAS_FMT_NS::format_to(
        result,
        DAS_CORE_FOREIGNINTERFACEHOST_VAR(enum_descriptions));
    result = DAS_FMT_NS::format_to(
        result,
        DAS_CORE_FOREIGNINTERFACEHOST_VAR(deprecation_message));
    result =
        DAS_FMT_NS::format_to(result, DAS_CORE_FOREIGNINTERFACEHOST_VAR(type));

    return result;
}

auto(DAS_FMT_NS::formatter<DAS::Core::ForeignInterfaceHost::PluginDesc, char>::
         format)(
    const DAS::Core::ForeignInterfaceHost::PluginDesc& desc,
    format_context&                                    ctx) const ->
    typename std::remove_reference_t<decltype(ctx)>::iterator
{
    auto result = ctx.out();

    DAS_FMT_NS::format_to(result, DAS_CORE_FOREIGNINTERFACEHOST_VAR(language));
    DAS_FMT_NS::format_to(result, DAS_CORE_FOREIGNINTERFACEHOST_VAR(name));
    DAS_FMT_NS::format_to(
        result,
        DAS_CORE_FOREIGNINTERFACEHOST_VAR(description));
    DAS_FMT_NS::format_to(result, DAS_CORE_FOREIGNINTERFACEHOST_VAR(author));
    DAS_FMT_NS::format_to(result, DAS_CORE_FOREIGNINTERFACEHOST_VAR(version));
    DAS_FMT_NS::format_to(
        result,
        DAS_CORE_FOREIGNINTERFACEHOST_VAR(supported_system));
    DAS_FMT_NS::format_to(
        result,
        DAS_CORE_FOREIGNINTERFACEHOST_VAR(plugin_filename_extension));
    DAS_FMT_NS::format_to(result, DAS_CORE_FOREIGNINTERFACEHOST_VAR(guid));
    DAS_FMT_NS::format_to(
        result,
        DAS_CORE_FOREIGNINTERFACEHOST_VAR(settings_desc));

    return result;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace Details
{
    template <class T>
    void OptionalFromJson(
        const nlohmann::json& json,
        const char*           key,
        std::optional<T>&     opt_value)
    {
        const auto it = json.find(key);
        if (it != json.end())
        {
            opt_value = it->template get<T>();
        }
        else
        {
            opt_value = std::nullopt;
        }
    }

    template <class T>
    void OptionalToJson(
        nlohmann::json&         json,
        const char*             key,
        const std::optional<T>& opt_value)
    {
        if (opt_value)
        {
            json[key] = opt_value.value();
        }
    }
}

void from_json(const nlohmann::json& input, PluginSettingDesc& output)
{
    DAS_CORE_TRACE_SCOPE;

    input.at("name").get_to(output.name);
    input.at("type").get_to(output.type);
    switch (output.type)
    {
    case DAS_TYPE_BOOL:
        output.default_value = input.at("defaultValue").get<bool>();
        break;
    case DAS_TYPE_INT:
        output.default_value = input.at("defaultValue").get<std::int64_t>();
        break;
    case DAS_TYPE_FLOAT:
        output.default_value = input.at("defaultValue").get<float>();
        break;
    case DAS_TYPE_STRING:
        output.default_value = input.at("defaultValue").get<std::string>();
        break;
    default:
        throw Utils::UnexpectedEnumException::FromEnum(output.type);
    }
    Details::OptionalFromJson(input, "description", output.description);
    Details::OptionalFromJson(input, "enumValues", output.enum_values);
    Details::OptionalFromJson(
        input,
        "enumDescriptions",
        output.enum_descriptions);
    Details::OptionalFromJson(
        input,
        "deprecation_message",
        output.deprecation_message);
}

void PluginDesc::SettingsJson::SetValue(IDasReadOnlyString* p_json)
{
    std::lock_guard _{mutex_};
    settings_json_ = p_json;
}

void PluginDesc::SettingsJson::GetValue(IDasReadOnlyString** pp_out_json)
{
    std::lock_guard _{mutex_};
    Utils::SetResult(settings_json_, pp_out_json);
}

void from_json(const nlohmann::json& input, PluginDesc& output)
{
    DAS_CORE_TRACE_SCOPE;

    input.at("language").get_to(output.language);
    input.at("name").get_to(output.name);
    input.at("description").get_to(output.description);
    input.at("author").get_to(output.author);
    input.at("version").get_to(output.version);
    input.at("supportedSystem").get_to(output.supported_system);
    input.at("pluginFilenameExtension")
        .get_to(output.plugin_filename_extension);
    if (const auto it_resource_path = input.find("resourcePath");
        it_resource_path != input.end())
    {
        output.opt_resource_path = it_resource_path->get<std::string>();
    }
    else
    {
        output.opt_resource_path =
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR("resource");
    }
    const auto guid_string = input.at("guid").get<std::string>();
    output.guid = MakeDasGuid(guid_string);
    if (const auto it_settings = input.find("settings");
        it_settings == input.end())
    {
        return;
    }
    const auto& settings = input.at("settings");
    settings.get_to(output.settings_desc);
    output.settings_desc_json = settings.dump();
    output.default_settings = nlohmann::json{};
    for (const auto& setting : output.settings_desc)
    {
        switch (setting.type)
        {
        case DAS_TYPE_BOOL:
            output.default_settings[output.name] =
                std::get<bool>(setting.default_value);
            break;
        case DAS_TYPE_INT:
            output.default_settings[output.name] =
                std::get<std::int64_t>(setting.default_value);
            break;
        case DAS_TYPE_FLOAT:
            output.default_settings[output.name] =
                std::get<float>(setting.default_value);
            break;
        case DAS_TYPE_STRING:
            output.default_settings[output.name] =
                std::get<std::string>(setting.default_value);
            break;
        default:
            DAS_CORE_LOG_ERROR(
                "Unexpected enum value. Setting name = {}, value = {}.",
                setting.name,
                setting.type);
            throw Utils::UnexpectedEnumException::FromEnum(setting.type);
        }
    }
    const DasReadOnlyStringWrapper default_settings_json{
        output.default_settings.dump()};
    output.settings_json_->SetValue(default_settings_json.Get());
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
