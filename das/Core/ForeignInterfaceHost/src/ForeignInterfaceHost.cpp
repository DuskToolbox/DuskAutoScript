// clang-format off
#include <das/DasConfig.h>
// clang-format on
#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>

#include <algorithm>
#include <cctype>
#include <das/Utils/StringUtils.h>
#include <das/Utils/UnexpectedEnumException.h>
#include <das/_autogen/idl/abi/DasSettings.h>
#include <iterator>
#include <magic_enum_format.hpp>
#include <stdexcept>
#include <unordered_set>

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
    T JsonValueToYyjsonScalar(const yyjson::writer::const_value_ref& v);

    template <>
    inline bool JsonValueToYyjsonScalar<bool>(
        const yyjson::writer::const_value_ref& v)
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
        const yyjson::writer::const_value_ref& v)
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
        const yyjson::writer::const_value_ref& v)
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
        const yyjson::writer::const_value_ref& v)
    {
        auto opt = v.as_string();
        if (!opt)
        {
            throw std::runtime_error("Expected string value");
        }
        return std::string(*opt);
    }

    template <>
    inline Das::ExportInterface::DasType JsonValueToYyjsonScalar<
        Das::ExportInterface::DasType>(const yyjson::writer::const_value_ref& v)
    {
        auto opt = v.as_sint();
        if (opt)
        {
            return static_cast<Das::ExportInterface::DasType>(*opt);
        }

        auto str = v.as_string();
        if (!str)
        {
            throw std::runtime_error("Expected DasType value");
        }

        std::string normalized;
        normalized.reserve(str->size());
        for (const auto ch : *str)
        {
            if (ch != '_')
            {
                normalized.push_back(
                    static_cast<char>(
                        std::tolower(static_cast<unsigned char>(ch))));
            }
        }

        using Das::ExportInterface::DAS_TYPE_BOOL;
        using Das::ExportInterface::DAS_TYPE_FLOAT;
        using Das::ExportInterface::DAS_TYPE_INT;
        using Das::ExportInterface::DAS_TYPE_JSON_ARRAY;
        using Das::ExportInterface::DAS_TYPE_JSON_OBJECT;
        using Das::ExportInterface::DAS_TYPE_STRING;
        using Das::ExportInterface::DAS_TYPE_UINT;

        if (normalized == "bool")
        {
            return DAS_TYPE_BOOL;
        }
        if (normalized == "float")
        {
            return DAS_TYPE_FLOAT;
        }
        if (normalized == "int")
        {
            return DAS_TYPE_INT;
        }
        if (normalized == "jsonarray")
        {
            return DAS_TYPE_JSON_ARRAY;
        }
        if (normalized == "jsonobject")
        {
            return DAS_TYPE_JSON_OBJECT;
        }
        if (normalized == "string")
        {
            return DAS_TYPE_STRING;
        }
        if (normalized == "uint")
        {
            return DAS_TYPE_UINT;
        }

        throw std::runtime_error(
            DAS_FMT_NS::format("Invalid DasType value: {}", *str));
    }

    inline void OptionalFieldFromYyjson(
        const yyjson::writer::const_object_ref& obj,
        std::string_view                        key,
        std::optional<std::string>&             opt_value)
    {
        if (!obj.contains(key))
        {
            opt_value = std::nullopt;
            return;
        }
        auto field = obj[std::string_view(key)];
        if (!field.is_null())
        {
            opt_value = JsonValueToYyjsonScalar<std::string>(field);
        }
        else
        {
            opt_value = std::nullopt;
        }
    }

    inline void OptionalFieldFromYyjson(
        const yyjson::writer::const_object_ref&  obj,
        std::string_view                         key,
        std::optional<std::vector<std::string>>& opt_value)
    {
        if (!obj.contains(key))
        {
            opt_value = std::nullopt;
            return;
        }
        auto field = obj[std::string_view(key)];
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
            auto opt = elem.as_string();
            if (opt)
            {
                result.emplace_back(*opt);
            }
        }
        opt_value = std::move(result);
    }

    inline LoadMode LoadModeFromString(std::string_view value)
    {
        std::string normalized;
        normalized.reserve(value.size());
        for (const auto ch : value)
        {
            if (ch != '_')
            {
                normalized.push_back(
                    static_cast<char>(
                        std::tolower(static_cast<unsigned char>(ch))));
            }
        }

        if (normalized == "ipc")
        {
            return LoadMode::Ipc;
        }
        if (normalized == "inprocess")
        {
            return LoadMode::InProcess;
        }

        throw std::runtime_error(
            DAS_FMT_NS::format("Invalid loadMode value: {}", value));
    }

} // namespace Details

namespace
{
    yyjson::value CopyJsonValue(
        const yyjson::writer::const_value_ref& value)
    {
        yyjson::value result;
        result = value;
        return result;
    }

    std::optional<DasGuid> TryMakeGuid(std::string_view value)
    {
        try
        {
            return MakeDasGuid(value);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    std::string JoinReasons(const std::vector<std::string>& reasons)
    {
        std::string result;
        for (std::size_t i = 0; i < reasons.size(); ++i)
        {
            if (i != 0)
            {
                result += "; ";
            }
            result += reasons[i];
        }
        return result;
    }

    std::optional<std::string> OptionalStringFromObject(
        const yyjson::writer::const_object_ref& obj,
        std::string_view                        key)
    {
        if (!obj.contains(key))
        {
            return std::nullopt;
        }

        auto field = obj[key];
        auto value = field.as_string();
        if (!value)
        {
            return std::nullopt;
        }
        return std::string(*value);
    }

    std::string ComponentPath(std::string_view component_key)
    {
        return DAS_FMT_NS::format(
            "taskComponents.components.{}",
            component_key);
    }

    std::vector<std::string> JsonStringArrayToVector(
        const yyjson::writer::const_value_ref& value)
    {
        std::vector<std::string> result;
        if (!value.is_array())
        {
            return result;
        }

        auto arr = *value.as_array();
        for (const auto& elem : arr)
        {
            auto str = elem.as_string();
            if (str)
            {
                result.emplace_back(*str);
            }
        }
        return result;
    }

    void ParseTaskComponentsFromJson(
        const yyjson::writer::const_object_ref& obj,
        TaskComponentsManifestDesc&             output)
    {
        if (obj.contains(std::string_view("factories")))
        {
            auto factories_val = obj[std::string_view("factories")];
            auto factories_arr = factories_val.as_array();
            if (factories_arr)
            {
                std::vector<std::string> factories;
                for (const auto& elem : *factories_arr)
                {
                    auto factory_guid = elem.as_string();
                    factories.emplace_back(
                        factory_guid ? std::string(*factory_guid)
                                     : std::string{});
                }
                output.factories = std::move(factories);
            }
        }

        if (obj.contains(std::string_view("components")))
        {
            auto components_val = obj[std::string_view("components")];
            auto components_obj = components_val.as_object();
            if (components_obj)
            {
                std::unordered_map<
                    std::string,
                    TaskComponentManifestEntryDesc>
                    components;
                for (const auto& [key, value] : *components_obj)
                {
                    TaskComponentManifestEntryDesc entry;
                    auto entry_obj = value.as_object();
                    if (entry_obj)
                    {
                        entry.factory_guid = OptionalStringFromObject(
                            *entry_obj,
                            "factoryGuid");
                        if (entry_obj->contains(std::string_view("definition")))
                        {
                            entry.definition = CopyJsonValue(
                                (*entry_obj)[std::string_view("definition")]);
                        }
                    }
                    components.emplace(std::string(key), std::move(entry));
                }
                output.components = std::move(components);
            }
        }
    }
} // namespace

void ParsePluginSettingDescFromJson(
    const yyjson::writer::const_object_ref& obj,
    PluginSettingDesc&                      output)
{
    DAS_CORE_TRACE_SCOPE;

    output.name = Details::JsonValueToYyjsonScalar<std::string>(
        obj[std::string_view("name")]);
    output.type =
        Details::JsonValueToYyjsonScalar<Das::ExportInterface::DasType>(
            obj[std::string_view("type")]);

    switch (output.type)
    {
    case Das::ExportInterface::DAS_TYPE_BOOL:
        output.default_value = Details::JsonValueToYyjsonScalar<bool>(
            obj[std::string_view("defaultValue")]);
        break;
    case Das::ExportInterface::DAS_TYPE_INT:
        output.default_value = Details::JsonValueToYyjsonScalar<std::int64_t>(
            obj[std::string_view("defaultValue")]);
        break;
    case Das::ExportInterface::DAS_TYPE_FLOAT:
        output.default_value = Details::JsonValueToYyjsonScalar<float>(
            obj[std::string_view("defaultValue")]);
        break;
    case Das::ExportInterface::DAS_TYPE_STRING:
        output.default_value = Details::JsonValueToYyjsonScalar<std::string>(
            obj[std::string_view("defaultValue")]);
        break;
    default:
        throw Utils::UnexpectedEnumException::FromEnum(output.type);
    }

    Details::OptionalFieldFromYyjson(obj, "description", output.description);
    Details::OptionalFieldFromYyjson(obj, "enumValues", output.enum_values);
    Details::OptionalFieldFromYyjson(
        obj,
        "enumDescriptions",
        output.enum_descriptions);
    Details::OptionalFieldFromYyjson(
        obj,
        "deprecationMessage",
        output.deprecation_message);

    if (obj.contains(std::string_view("required")))
    {
        auto required_val = obj[std::string_view("required")];
        if (!required_val.is_null())
        {
            auto opt = required_val.as_bool();
            if (opt)
            {
                output.required = *opt;
            }
        }
    }
}

void ParsePluginSettingsGroupFromJson(
    const yyjson::writer::const_object_ref&           obj,
    std::unordered_map<DasGuid, PluginSettingsGroup>& output)
{
    DAS_CORE_TRACE_SCOPE;

    for (const auto& [key, value] : obj)
    {
        const auto          guid = MakeDasGuid(std::string(key));
        PluginSettingsGroup group;
        auto                val_obj = value.as_object();
        if (!val_obj)
        {
            continue;
        }
        const auto& sub_obj = *val_obj;
        group.name = Details::JsonValueToYyjsonScalar<std::string>(
            sub_obj[std::string_view("name")]);
        group.description = Details::JsonValueToYyjsonScalar<std::string>(
            sub_obj[std::string_view("description")]);
        auto descriptors_field = sub_obj[std::string_view("descriptors")];
        if (!descriptors_field.is_null())
        {
            auto desc_arr = *descriptors_field.as_array();
            for (const auto& elem : desc_arr)
            {
                auto elem_obj = elem.as_object();
                if (!elem_obj)
                {
                    continue;
                }
                PluginSettingDesc desc;
                ParsePluginSettingDescFromJson(*elem_obj, desc);
                group.descriptors.push_back(std::move(desc));
            }
        }
        output.emplace(guid, std::move(group));
    }
}

void ParseTaskDescriptorFromJson(
    const yyjson::writer::const_object_ref& obj,
    TaskDescriptor&                         output)
{
    DAS_CORE_TRACE_SCOPE;

    auto plugin_guid_str = Details::JsonValueToYyjsonScalar<std::string>(
        obj[std::string_view("pluginGuid")]);
    output.plugin_guid = MakeDasGuid(plugin_guid_str);
    output.name = Details::JsonValueToYyjsonScalar<std::string>(
        obj[std::string_view("name")]);
    output.description = Details::JsonValueToYyjsonScalar<std::string>(
        obj[std::string_view("description")]);
    Details::OptionalFieldFromYyjson(obj, "gameName", output.game_name);
    if (obj.contains(std::string_view("authoring")))
    {
        auto authoring_val = obj[std::string_view("authoring")];
        auto authoring_obj = authoring_val.as_object();
        if (authoring_obj)
        {
            TaskAuthoringCapabilityDesc authoring;
            authoring.factory_guid = MakeDasGuid(
                Details::JsonValueToYyjsonScalar<std::string>(
                    (*authoring_obj)[std::string_view("factoryGuid")]));
            if (authoring_obj->contains(std::string_view("supportedKinds")))
            {
                authoring.supported_kinds = JsonStringArrayToVector(
                    (*authoring_obj)[std::string_view("supportedKinds")]);
            }
            output.authoring = std::move(authoring);
        }
    }
    if (obj.contains(std::string_view("descriptors")))
    {
        auto descriptors_field = obj[std::string_view("descriptors")];
        if (!descriptors_field.is_null())
        {
            auto desc_arr = *descriptors_field.as_array();
            for (const auto& elem : desc_arr)
            {
                auto elem_obj = elem.as_object();
                if (!elem_obj)
                {
                    continue;
                }
                PluginSettingDesc desc;
                ParsePluginSettingDescFromJson(*elem_obj, desc);
                output.descriptors.push_back(std::move(desc));
            }
        }
    }
}

TaskComponentsValidationResult ValidateTaskComponents(
    const TaskComponentsManifestDesc& task_components)
{
    TaskComponentsValidationResult result;
    std::unordered_set<DasGuid>     declared_factories;

    if (!task_components.factories.has_value())
    {
        result.rejection_reasons.emplace_back(
            "taskComponents.factories: missing required array");
    }
    else
    {
        const auto& factories = *task_components.factories;
        for (std::size_t i = 0; i < factories.size(); ++i)
        {
            auto factory_guid = TryMakeGuid(factories[i]);
            if (!factory_guid)
            {
                result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                    "taskComponents.factories[{}]: invalid factory GUID",
                    i));
                continue;
            }
            declared_factories.insert(*factory_guid);
        }
    }

    if (!task_components.components.has_value())
    {
        result.rejection_reasons.emplace_back(
            "taskComponents.components: missing required object");
        return result;
    }

    for (const auto& [component_key, entry] : *task_components.components)
    {
        const auto component_path = ComponentPath(component_key);
        const auto component_guid = TryMakeGuid(component_key);
        if (!component_guid)
        {
            result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                "{}: invalid component GUID key",
                component_path));
        }

        std::optional<DasGuid> entry_factory_guid;
        if (!entry.factory_guid.has_value() || entry.factory_guid->empty())
        {
            result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                "{}.factoryGuid: missing required string",
                component_path));
        }
        else
        {
            entry_factory_guid = TryMakeGuid(*entry.factory_guid);
            if (!entry_factory_guid)
            {
                result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                    "{}.factoryGuid: invalid factory GUID",
                    component_path));
            }
            else if (
                task_components.factories.has_value()
                && !declared_factories.contains(*entry_factory_guid))
            {
                result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                    "{}.factoryGuid: undeclared factory GUID",
                    component_path));
            }
        }

        if (!entry.definition.has_value())
        {
            result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                "{}.definition: missing required object",
                component_path));
            continue;
        }

        auto definition_obj = entry.definition->as_object();
        if (!definition_obj)
        {
            result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                "{}.definition: expected object",
                component_path));
            continue;
        }

        if (!definition_obj->contains(std::string_view("componentGuid")))
        {
            result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                "{}.definition.componentGuid: missing required string",
                component_path));
        }
        else
        {
            auto definition_component_guid =
                (*definition_obj)[std::string_view("componentGuid")]
                    .as_string();
            if (!definition_component_guid)
            {
                result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                    "{}.definition.componentGuid: missing required string",
                    component_path));
            }
            else
            {
                auto parsed_definition_guid =
                    TryMakeGuid(*definition_component_guid);
                if (!parsed_definition_guid)
                {
                    result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                        "{}.definition.componentGuid: invalid component GUID",
                        component_path));
                }
                else if (
                    component_guid
                    && *parsed_definition_guid != *component_guid)
                {
                    result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                        "{}.definition.componentGuid: must match component key",
                        component_path));
                }
            }
        }

        if (!definition_obj->contains(std::string_view("schemaVersion"))
            || (*definition_obj)[std::string_view("schemaVersion")].is_null())
        {
            result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                "{}.definition.schemaVersion: missing required field",
                component_path));
        }

        if (!definition_obj->contains(std::string_view("kind"))
            || !(*definition_obj)[std::string_view("kind")].as_string())
        {
            result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                "{}.definition.kind: missing required string",
                component_path));
        }

        if (!definition_obj->contains(std::string_view("inputs"))
            || !(*definition_obj)[std::string_view("inputs")].is_array())
        {
            result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                "{}.definition.inputs: expected array",
                component_path));
        }

        if (!definition_obj->contains(std::string_view("outputs"))
            || !(*definition_obj)[std::string_view("outputs")].is_array())
        {
            result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                "{}.definition.outputs: expected array",
                component_path));
        }

        if (definition_obj->contains(std::string_view("config")))
        {
            auto config = (*definition_obj)[std::string_view("config")];
            if (!config.is_object())
            {
                result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                    "{}.definition.config: expected object",
                    component_path));
            }
        }

        if (definition_obj->contains(std::string_view("diagnostics")))
        {
            auto diagnostics =
                (*definition_obj)[std::string_view("diagnostics")];
            if (!diagnostics.is_array())
            {
                result.rejection_reasons.emplace_back(DAS_FMT_NS::format(
                    "{}.definition.diagnostics: expected array",
                    component_path));
            }
        }
    }

    return result;
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
    const yyjson::writer::const_object_ref& obj,
    PluginPackageDesc&                      output)
{
    DAS_CORE_TRACE_SCOPE;

    auto lang_val = obj[std::string_view("language")];
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

    output.load_mode = LoadMode::InProcess;
    if (obj.contains(std::string_view("loadMode")))
    {
        auto load_mode_val = obj[std::string_view("loadMode")];
        if (!load_mode_val.is_null())
        {
            auto lmo = load_mode_val.as_sint();
            if (lmo)
            {
                output.load_mode = static_cast<LoadMode>(*lmo);
            }
            else
            {
                auto load_mode_str = load_mode_val.as_string();
                if (!load_mode_str)
                {
                    throw std::runtime_error(
                        "Expected numeric or string loadMode value");
                }
                output.load_mode = Details::LoadModeFromString(*load_mode_str);
            }
        }
    }

    output.name = Details::JsonValueToYyjsonScalar<std::string>(
        obj[std::string_view("name")]);
    output.description = Details::JsonValueToYyjsonScalar<std::string>(
        obj[std::string_view("description")]);
    output.author = Details::JsonValueToYyjsonScalar<std::string>(
        obj[std::string_view("author")]);
    output.version = Details::JsonValueToYyjsonScalar<std::string>(
        obj[std::string_view("version")]);
    output.supported_system = Details::JsonValueToYyjsonScalar<std::string>(
        obj[std::string_view("supportedSystem")]);
    output.plugin_filename_extension =
        Details::JsonValueToYyjsonScalar<std::string>(
            obj[std::string_view("pluginFilenameExtension")]);

    output.opt_resource_path = DAS_UTILS_STRINGUTILS_DEFINE_U8STR("resource");
    if (obj.contains(std::string_view("resourcePath")))
    {
        auto resource_path_val = obj[std::string_view("resourcePath")];
        if (!resource_path_val.is_null())
        {
            auto opt = resource_path_val.as_string();
            if (!opt)
            {
                throw std::runtime_error("Expected string resourcePath value");
            }
            output.opt_resource_path = std::string(*opt);
        }
    }

    auto guid_str = Details::JsonValueToYyjsonScalar<std::string>(
        obj[std::string_view("guid")]);
    output.guid = MakeDasGuid(guid_str);

    // Parse "settings" field: support both legacy array and new
    // plugin-GUID-keyed object format.
    if (obj.contains(std::string_view("settings")))
    {
        auto settings_val = obj[std::string_view("settings")];
        if (!settings_val.is_null() && settings_val.is_array())
        {
            // Legacy flat array format.
            auto arr = *settings_val.as_array();
            for (const auto& elem : arr)
            {
                auto elem_obj = elem.as_object();
                if (!elem_obj)
                {
                    continue;
                }
                PluginSettingDesc desc;
                ParsePluginSettingDescFromJson(*elem_obj, desc);
                output.settings_desc.push_back(std::move(desc));
            }
            // Build default_settings as a yyjson value object
            output.default_settings = Das::Utils::MakeYyjsonObject();
            auto ds_obj = output.default_settings.as_object();
            for (const auto& setting : output.settings_desc)
            {
                if (!ds_obj)
                {
                    break;
                }
                switch (setting.type)
                {
                case Das::ExportInterface::DAS_TYPE_BOOL:
                    (*ds_obj)[output.name] =
                        std::get<bool>(setting.default_value);
                    break;
                case Das::ExportInterface::DAS_TYPE_INT:
                    (*ds_obj)[output.name] =
                        std::get<std::int64_t>(setting.default_value);
                    break;
                case Das::ExportInterface::DAS_TYPE_FLOAT:
                    (*ds_obj)[output.name] =
                        std::get<float>(setting.default_value);
                    break;
                case Das::ExportInterface::DAS_TYPE_STRING:
                    (*ds_obj)[output.name] =
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
        else if (!settings_val.is_null() && settings_val.is_object())
        {
            // New plugin-GUID-keyed object format.
            auto settings_obj = settings_val.as_object();
            if (settings_obj)
            {
                ParsePluginSettingsGroupFromJson(
                    *settings_obj,
                    output.settings_groups);
            }
        }
    }

    if (obj.contains(std::string_view("taskComponents")))
    {
        auto task_components_val = obj[std::string_view("taskComponents")];
        auto task_components_obj = task_components_val.as_object();
        if (!task_components_obj)
        {
            throw std::runtime_error(
                "taskComponents: expected object");
        }

        TaskComponentsManifestDesc task_components;
        ParseTaskComponentsFromJson(*task_components_obj, task_components);

        auto validation = ValidateTaskComponents(task_components);
        if (!validation.IsValid())
        {
            throw std::runtime_error(DAS_FMT_NS::format(
                "Invalid taskComponents manifest: {}",
                JoinReasons(validation.rejection_reasons)));
        }

        output.task_components = std::move(task_components);
    }

    // Parse "tasks" field: task-GUID-keyed object.
    if (obj.contains(std::string_view("tasks")))
    {
        auto tasks_val = obj[std::string_view("tasks")];
        if (!tasks_val.is_null() && tasks_val.is_object())
        {
            auto tasks_obj = *tasks_val.as_object();
            for (const auto& [key, value] : tasks_obj)
            {
                const auto task_guid = MakeDasGuid(std::string(key));
                auto       task_obj = value.as_object();
                if (!task_obj)
                {
                    continue;
                }
                TaskDescriptor desc;
                ParseTaskDescriptorFromJson(*task_obj, desc);
                output.task_descriptors.emplace(task_guid, std::move(desc));
            }
        }
    }
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
