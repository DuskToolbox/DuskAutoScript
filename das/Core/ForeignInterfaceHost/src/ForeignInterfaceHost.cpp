// clang-format off
#include <das/DasConfig.h>
// clang-format on
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/EnumUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/UnexpectedEnumException.h>
#include <das/_autogen/idl/abi/DasSettings.h>
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
                Das::ExportInterface::DAS_TYPE_BOOL,
                bool);
            DAS_CORE_FOREIGNINTERFACEHOST_VERIFY_AND_FORMAT(
                Das::ExportInterface::DAS_TYPE_INT,
                std::int64_t);
            DAS_CORE_FOREIGNINTERFACEHOST_VERIFY_AND_FORMAT(
                Das::ExportInterface::DAS_TYPE_FLOAT,
                float);
            DAS_CORE_FOREIGNINTERFACEHOST_VERIFY_AND_FORMAT(
                Das::ExportInterface::DAS_TYPE_STRING,
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

auto(DAS_FMT_NS::formatter<
     DAS::Core::ForeignInterfaceHost::PluginPackageDesc,
     char>::format)(
    const DAS::Core::ForeignInterfaceHost::PluginPackageDesc& desc,
    format_context&                                           ctx) const ->
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
    T JsonValueToYyjsonScalar(
        const yyjson::writer::detail::value_ref& value_ref);

    template <>
    inline bool JsonValueToYyjsonScalar<bool>(
        const yyjson::writer::detail::value_ref& v)
    {
        auto opt = v.as_bool();
        if (!opt)
        {
            throw std::runtime_error("Expected bool value");
        }
        return *opt;
    }

    template <>
    inline std::int64_t JsonValueToYyjsonScalar<std::int64_t>(
        const yyjson::writer::detail::value_ref& v)
    {
        auto opt = v.as_sint();
        if (!opt)
        {
            throw std::runtime_error("Expected int64_t value");
        }
        return *opt;
    }

    template <>
    inline float JsonValueToYyjsonScalar<float>(
        const yyjson::writer::detail::value_ref& v)
    {
        auto opt = v.as_real();
        if (!opt)
        {
            throw std::runtime_error("Expected float value");
        }
        return static_cast<float>(*opt);
    }

    template <>
    inline std::string JsonValueToYyjsonScalar<std::string>(
        const yyjson::writer::detail::value_ref& v)
    {
        auto opt = v.as_string();
        if (!opt)
        {
            throw std::runtime_error("Expected string value");
        }
        return std::string(*opt);
    }

    template <>
    inline Das::ExportInterface::DasType
    JsonValueToYyjsonScalar<Das::ExportInterface::DasType>(
        const yyjson::writer::detail::value_ref& v)
    {
        auto opt = v.as_sint();
        if (!opt)
        {
            throw std::runtime_error("Expected DasType value");
        }
        return static_cast<Das::ExportInterface::DasType>(*opt);
    }

    inline void OptionalFromYyjson(
        const yyjson::writer::detail::value_ref& json_val,
        const std::string_view                   key,
        std::optional<bool>&                     opt_value)
    {
        auto field = json_val[key];
        if (!field.is_null())
        {
            opt_value = JsonValueToYyjsonScalar<bool>(field);
        }
        else
        {
            opt_value = std::nullopt;
        }
    }

    inline void OptionalFromYyjson(
        const yyjson::writer::detail::value_ref& json_val,
        const std::string_view                   key,
        std::optional<std::int64_t>&             opt_value)
    {
        auto field = json_val[key];
        if (!field.is_null())
        {
            opt_value = JsonValueToYyjsonScalar<std::int64_t>(field);
        }
        else
        {
            opt_value = std::nullopt;
        }
    }

    inline void OptionalFromYyjson(
        const yyjson::writer::detail::value_ref& json_val,
        const std::string_view                   key,
        std::optional<float>&                    opt_value)
    {
        auto field = json_val[key];
        if (!field.is_null())
        {
            opt_value = JsonValueToYyjsonScalar<float>(field);
        }
        else
        {
            opt_value = std::nullopt;
        }
    }

    inline void OptionalFromYyjson(
        const yyjson::writer::detail::value_ref& json_val,
        const std::string_view                   key,
        std::optional<std::string>&              opt_value)
    {
        auto field = json_val[key];
        if (!field.is_null())
        {
            opt_value = JsonValueToYyjsonScalar<std::string>(field);
        }
        else
        {
            opt_value = std::nullopt;
        }
    }

    inline void OptionalFromYyjson(
        const yyjson::writer::detail::value_ref& json_val,
        const std::string_view                   key,
        std::optional<std::vector<std::string>>& opt_value)
    {
        auto field = json_val[key];
        if (field.is_null())
        {
            opt_value = std::nullopt;
            return;
        }
        if (!field.is_array())
        {
            throw std::runtime_error("Expected array value");
        }
        std::vector<std::string> result;
        auto                     arr = *field.as_array();
        for (const auto& elem : arr)
        {
            auto opt = elem.template as_string();
            if (opt)
            {
                result.emplace_back(*opt);
            }
        }
        opt_value = std::move(result);
    }

} // namespace Details

void ParsePluginSettingDescFromJson(
    const yyjson::writer::detail::value_ref input,
    PluginSettingDesc&                      output)
{
    DAS_CORE_TRACE_SCOPE;

    output.name = Details::JsonValueToYyjsonScalar<std::string>(input["name"]);
    output.type =
        Details::JsonValueToYyjsonScalar<Das::ExportInterface::DasType>(
            input["type"]);
    switch (output.type)
    {
    case Das::ExportInterface::DAS_TYPE_BOOL:
        output.default_value =
            Details::JsonValueToYyjsonScalar<bool>(input["defaultValue"]);
        break;
    case Das::ExportInterface::DAS_TYPE_INT:
        output.default_value = Details::JsonValueToYyjsonScalar<std::int64_t>(
            input["defaultValue"]);
        break;
    case Das::ExportInterface::DAS_TYPE_FLOAT:
        output.default_value =
            Details::JsonValueToYyjsonScalar<float>(input["defaultValue"]);
        break;
    case Das::ExportInterface::DAS_TYPE_STRING:
        output.default_value = Details::JsonValueToYyjsonScalar<std::string>(
            input["defaultValue"]);
        break;
    default:
        throw Utils::UnexpectedEnumException::FromEnum(output.type);
    }
    Details::OptionalFromYyjson(input, "description", output.description);
    Details::OptionalFromYyjson(input, "enumValues", output.enum_values);
    Details::OptionalFromYyjson(
        input,
        "enumDescriptions",
        output.enum_descriptions);
    Details::OptionalFromYyjson(
        input,
        "deprecationMessage",
        output.deprecation_message);
    auto required_val = input["required"];
    if (!required_val.is_null())
    {
        auto opt = required_val.as_bool();
        if (opt)
        {
            output.required = *opt;
        }
    }
}

void ParsePluginSettingsGroupFromJson(
    const yyjson::writer::detail::value_ref           input,
    std::unordered_map<DasGuid, PluginSettingsGroup>& output)
{
    DAS_CORE_TRACE_SCOPE;

    if (!input.is_object())
    {
        return;
    }
    auto obj = *input.as_object();
    for (const auto& [key, value] : obj)
    {
        const auto          guid = MakeDasGuid(std::string(key));
        PluginSettingsGroup group;
        group.name =
            Details::JsonValueToYyjsonScalar<std::string>(value["name"]);
        group.description =
            Details::JsonValueToYyjsonScalar<std::string>(value["description"]);
        auto descriptors_field = value["descriptors"];
        if (!descriptors_field.is_null())
        {
            auto desc_arr = *descriptors_field.as_array();
            for (const auto& elem : desc_arr)
            {
                PluginSettingDesc desc;
                ParsePluginSettingDescFromJson(elem, desc);
                group.descriptors.push_back(std::move(desc));
            }
        }
        output.emplace(guid, std::move(group));
    }
}

void ParseTaskDescriptorFromJson(
    const yyjson::writer::detail::value_ref input,
    TaskDescriptor&                         output)
{
    DAS_CORE_TRACE_SCOPE;

    auto plugin_guid_str =
        Details::JsonValueToYyjsonScalar<std::string>(input["pluginGuid"]);
    output.plugin_guid = MakeDasGuid(plugin_guid_str);
    output.name = Details::JsonValueToYyjsonScalar<std::string>(input["name"]);
    output.description =
        Details::JsonValueToYyjsonScalar<std::string>(input["description"]);
    Details::OptionalFromYyjson(input, "gameName", output.game_name);
    auto descriptors_field = input["descriptors"];
    if (!descriptors_field.is_null())
    {
        auto desc_arr = *descriptors_field.as_array();
        for (const auto& elem : desc_arr)
        {
            PluginSettingDesc desc;
            ParsePluginSettingDescFromJson(elem, desc);
            output.descriptors.push_back(std::move(desc));
        }
    }
}

void PluginPackageDesc::SettingsJson::SetValue(IDasReadOnlyString* p_json)
{
    std::lock_guard _{mutex_};
    settings_json_ = p_json;
}

void PluginPackageDesc::SettingsJson::GetValue(IDasReadOnlyString** pp_out_json)
{
    std::lock_guard _{mutex_};
    Utils::SetResult(settings_json_, pp_out_json);
}

void ParsePluginPackageDescFromJson(
    const yyjson::writer::detail::value_ref input,
    PluginPackageDesc&                      output)
{
    DAS_CORE_TRACE_SCOPE;

    auto lang_val = input["language"];
    auto lang_opt = lang_val.as_sint();
    if (lang_opt)
    {
        output.language = static_cast<ForeignInterfaceLanguage>(*lang_opt);
    }
    else
    {
        auto lang_str_opt = lang_val.as_string();
        if (lang_str_opt)
        {
            // Try to parse language from string (for manifest files)
            auto lang_str = std::string(*lang_str_opt);
            if (lang_str == "Python")
            {
                output.language = ForeignInterfaceLanguage::Python;
            }
            else if (lang_str == "CSharp")
            {
                output.language = ForeignInterfaceLanguage::CSharp;
            }
            else if (lang_str == "Java")
            {
                output.language = ForeignInterfaceLanguage::Java;
            }
            else if (lang_str == "Cpp")
            {
                output.language = ForeignInterfaceLanguage::Cpp;
            }
            else if (lang_str == "Lua")
            {
                output.language = ForeignInterfaceLanguage::Lua;
            }
        }
    }

    auto load_mode_val = input["loadMode"];
    if (!load_mode_val.is_null())
    {
        auto lmo = load_mode_val.as_sint();
        if (lmo)
        {
            output.load_mode = static_cast<LoadMode>(*lmo);
        }
    }
    else
    {
        output.load_mode = LoadMode::InProcess;
    }

    output.name = Details::JsonValueToYyjsonScalar<std::string>(input["name"]);
    output.description =
        Details::JsonValueToYyjsonScalar<std::string>(input["description"]);
    output.author =
        Details::JsonValueToYyjsonScalar<std::string>(input["author"]);
    output.version =
        Details::JsonValueToYyjsonScalar<std::string>(input["version"]);
    output.supported_system =
        Details::JsonValueToYyjsonScalar<std::string>(input["supportedSystem"]);
    output.plugin_filename_extension =
        Details::JsonValueToYyjsonScalar<std::string>(
            input["pluginFilenameExtension"]);

    auto resource_path_val = input["resourcePath"];
    if (!resource_path_val.is_null())
    {
        auto opt = resource_path_val.as_string();
        if (opt)
        {
            output.opt_resource_path = std::string(*opt);
        }
    }
    else
    {
        output.opt_resource_path =
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR("resource");
    }

    auto guid_str =
        Details::JsonValueToYyjsonScalar<std::string>(input["guid"]);
    output.guid = MakeDasGuid(guid_str);

    // Parse "settings" field: support both legacy array and new
    // plugin-GUID-keyed object format.
    auto settings_val = input["settings"];
    if (!settings_val.is_null())
    {
        if (settings_val.is_array())
        {
            // Legacy flat array format.
            auto arr = *settings_val.as_array();
            for (const auto& elem : arr)
            {
                PluginSettingDesc desc;
                ParsePluginSettingDescFromJson(elem, desc);
                output.settings_desc.push_back(std::move(desc));
            }
            auto serialized = Das::Utils::SerializeYyjsonValue(
                yyjson::writer::detail::value{
                    yyjson::writer::detail::value_ref(settings_val)},
                false);
            if (serialized)
            {
                output.settings_desc_json = *serialized;
            }
            // Build default_settings object
            output.default_settings = yyjson::writer::detail::value{
                yyjson::construct_object_type_t{}};
            for (const auto& setting : output.settings_desc)
            {
                switch (setting.type)
                {
                case Das::ExportInterface::DAS_TYPE_BOOL:
                    output.default_settings[output.name] =
                        std::get<bool>(setting.default_value);
                    break;
                case Das::ExportInterface::DAS_TYPE_INT:
                    output.default_settings[output.name] =
                        std::get<std::int64_t>(setting.default_value);
                    break;
                case Das::ExportInterface::DAS_TYPE_FLOAT:
                    output.default_settings[output.name] =
                        std::get<float>(setting.default_value);
                    break;
                case Das::ExportInterface::DAS_TYPE_STRING:
                    output.default_settings[output.name] =
                        std::get<std::string>(setting.default_value);
                    break;
                default:
                    DAS_CORE_LOG_ERROR(
                        "Unexpected enum value. Setting name = {}, value = {}.",
                        setting.name,
                        setting.type);
                    throw Utils::UnexpectedEnumException::FromEnum(
                        setting.type);
                }
            }
            auto ds_serialized = Das::Utils::SerializeYyjsonValue(
                output.default_settings,
                false);
            if (ds_serialized)
            {
                DasPtr<IDasReadOnlyString> p_str;
                auto cr = CreateIDasReadOnlyStringFromUtf8(
                    ds_serialized->c_str(),
                    p_str.Put());
                if (DAS::IsOk(cr))
                {
                    output.settings_json_->SetValue(p_str.Get());
                }
            }
        }
        else if (settings_val.is_object())
        {
            // New plugin-GUID-keyed object format.
            ParsePluginSettingsGroupFromJson(
                settings_val,
                output.settings_groups);
            auto serialized = Das::Utils::SerializeYyjsonValue(
                yyjson::writer::detail::value{
                    yyjson::writer::detail::value_ref(settings_val)},
                false);
            if (serialized)
            {
                output.settings_desc_json = *serialized;
            }
        }
    }

    // Parse "tasks" field: task-GUID-keyed object.
    auto tasks_val = input["tasks"];
    if (!tasks_val.is_null() && tasks_val.is_object())
    {
        auto tasks_obj = *tasks_val.as_object();
        for (const auto& [key, value] : tasks_obj)
        {
            const auto     task_guid = MakeDasGuid(std::string(key));
            TaskDescriptor desc;
            ParseTaskDescriptorFromJson(value, desc);
            output.task_descriptors.emplace(task_guid, std::move(desc));
        }
    }
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
