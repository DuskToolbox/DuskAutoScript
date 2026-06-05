#include <das/Core/ForeignInterfaceHost/CSharpManifest.h>

#include <das/Core/Utils/StringUtils.h>

#include <algorithm>
#include <cctype>
#include <string>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    std::string ToLower(std::string_view value)
    {
        std::string lowered;
        lowered.reserve(value.size());
        for (const char ch : value)
        {
            lowered +=
                static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return lowered;
    }

    bool StartsWith(std::string_view value, std::string_view prefix)
    {
        return value.size() >= prefix.size()
               && value.substr(0, prefix.size()) == prefix;
    }

    bool IsModernDotNetTfm(std::string_view lowered)
    {
        if (!StartsWith(lowered, "net") || lowered.size() <= 3)
        {
            return false;
        }

        std::uint32_t major = 0;
        bool          has_digit = false;
        for (std::size_t index = 3; index < lowered.size(); ++index)
        {
            const auto ch = lowered[index];
            if (!std::isdigit(static_cast<unsigned char>(ch)))
            {
                break;
            }

            has_digit = true;
            major = (major * 10) + static_cast<std::uint32_t>(ch - '0');
        }

        return has_digit && major >= 5;
    }

    bool IsNetFxTfm(std::string_view lowered)
    {
        if (lowered == ".net framework" || lowered == "netframework"
            || lowered == "net framework" || lowered == "net4x")
        {
            return true;
        }

        if (StartsWith(lowered, "net4") && lowered.size() >= 4)
        {
            return true;
        }

        return false;
    }

    bool HasInvalidEntryPointToken(std::string_view value)
    {
        return value.find('+') != std::string_view::npos
               || value.find('`') != std::string_view::npos
               || value.find('<') != std::string_view::npos
               || value.find('>') != std::string_view::npos
               || value.find('(') != std::string_view::npos
               || value.find(')') != std::string_view::npos
               || value.find(':') != std::string_view::npos
               || value.find('*') != std::string_view::npos
               || value.find("..") != std::string_view::npos;
    }

    Das::Utils::Expected<std::string> ReadRequiredString(
        const yyjson::writer::const_object_ref& obj,
        std::string_view                        field_name,
        DasResult                               error_code)
    {
        if (!obj.contains(field_name))
        {
            return tl::make_unexpected(error_code);
        }

        const auto value = obj[field_name];
        const auto string_value = value.as_string();
        if (!string_value)
        {
            return tl::make_unexpected(error_code);
        }

        return std::string{*string_value};
    }

    Das::Utils::Expected<std::optional<std::string>> ReadOptionalString(
        const yyjson::writer::const_object_ref& obj,
        std::string_view                        field_name,
        DasResult                               error_code)
    {
        if (!obj.contains(field_name))
        {
            return std::optional<std::string>{};
        }

        const auto value = obj[field_name];
        if (value.is_null())
        {
            return std::optional<std::string>{};
        }

        const auto string_value = value.as_string();
        if (!string_value)
        {
            return tl::make_unexpected(error_code);
        }

        return std::optional<std::string>{std::string{*string_value}};
    }

    bool IsUnsafePackageRelativePath(const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute())
        {
            return true;
        }

        const auto normalized = path.lexically_normal();
        for (const auto& part : normalized)
        {
            if (part == "..")
            {
                return true;
            }
        }

        return false;
    }
} // namespace

Das::Utils::Expected<CSharpTargetFrameworkFamily> ClassifyCSharpTargetFramework(
    std::string_view target_framework)
{
    const auto lowered = ToLower(target_framework);
    if (IsNetFxTfm(lowered))
    {
        return CSharpTargetFrameworkFamily::NetFx;
    }

    if (IsModernDotNetTfm(lowered))
    {
        return CSharpTargetFrameworkFamily::ModernDotNet;
    }

    return tl::make_unexpected(DAS_E_CSHARP_UNSUPPORTED_TFM);
}

Das::Utils::Expected<CSharpEntryPoint> SplitCSharpEntryPoint(
    std::string_view entry_point)
{
    const auto last_dot = entry_point.rfind('.');
    if (last_dot == std::string_view::npos)
    {
        return tl::make_unexpected(DAS_E_CSHARP_ENTRYPOINT_INVALID);
    }

    const auto type_name = entry_point.substr(0, last_dot);
    const auto method_name = entry_point.substr(last_dot + 1);
    if (type_name.empty() || method_name.empty())
    {
        return tl::make_unexpected(DAS_E_CSHARP_ENTRYPOINT_INVALID);
    }

    if (HasInvalidEntryPointToken(type_name)
        || HasInvalidEntryPointToken(method_name))
    {
        return tl::make_unexpected(DAS_E_CSHARP_ENTRYPOINT_INVALID);
    }

    return CSharpEntryPoint{
        .type_name = std::string{type_name},
        .method_name = std::string{method_name},
    };
}

Das::Utils::Expected<CSharpManifest> ParseCSharpManifest(
    const std::filesystem::path& manifest_path,
    std::string_view             manifest_json)
{
    const auto manifest =
        Das::Utils::ParseYyjsonFromString(std::string{manifest_json});
    if (!manifest)
    {
        return tl::make_unexpected(DAS_E_CSHARP_MANIFEST_INVALID);
    }

    const auto obj = manifest->as_object();
    if (!obj)
    {
        return tl::make_unexpected(DAS_E_CSHARP_MANIFEST_INVALID);
    }

    auto name = ReadRequiredString(*obj, "name", DAS_E_CSHARP_MANIFEST_INVALID);
    if (!name)
    {
        return tl::make_unexpected(name.error());
    }

    auto plugin_filename_extension = ReadRequiredString(
        *obj,
        "pluginFilenameExtension",
        DAS_E_CSHARP_MANIFEST_INVALID);
    if (!plugin_filename_extension)
    {
        return tl::make_unexpected(plugin_filename_extension.error());
    }

    if (plugin_filename_extension->empty()
        || plugin_filename_extension->front() == '.')
    {
        return tl::make_unexpected(DAS_E_CSHARP_MANIFEST_INVALID);
    }

    auto target_framework = ReadRequiredString(
        *obj,
        "targetFramework",
        DAS_E_CSHARP_MANIFEST_INVALID);
    if (!target_framework)
    {
        return tl::make_unexpected(target_framework.error());
    }

    auto entry_point_string =
        ReadRequiredString(*obj, "entryPoint", DAS_E_CSHARP_ENTRYPOINT_INVALID);
    if (!entry_point_string)
    {
        return tl::make_unexpected(entry_point_string.error());
    }

    auto target_framework_family =
        ClassifyCSharpTargetFramework(*target_framework);
    if (!target_framework_family)
    {
        return tl::make_unexpected(target_framework_family.error());
    }

    auto entry_point = SplitCSharpEntryPoint(*entry_point_string);
    if (!entry_point)
    {
        return tl::make_unexpected(entry_point.error());
    }

    auto runtime_config_path_string = ReadOptionalString(
        *obj,
        "runtimeConfigPath",
        DAS_E_CSHARP_MANIFEST_INVALID);
    if (!runtime_config_path_string)
    {
        return tl::make_unexpected(runtime_config_path_string.error());
    }

    CSharpManifest result{};
    result.manifest_path = manifest_path;
    result.package_root = manifest_path.parent_path();
    result.name = *name;
    result.plugin_filename_extension = *plugin_filename_extension;
    result.target_framework = *target_framework;
    result.target_framework_family = *target_framework_family;
    result.entry_point = *entry_point;
    result.plugin_binary_path =
        result.package_root
        / (result.name + "." + result.plugin_filename_extension);

    if (result.target_framework_family
        == CSharpTargetFrameworkFamily::ModernDotNet)
    {
        std::filesystem::path runtime_config_path;
        if (*runtime_config_path_string)
        {
            runtime_config_path =
                std::filesystem::path{**runtime_config_path_string};
            if (IsUnsafePackageRelativePath(runtime_config_path))
            {
                return tl::make_unexpected(DAS_E_CSHARP_MANIFEST_INVALID);
            }
        }
        else
        {
            runtime_config_path = result.name + ".runtimeconfig.json";
        }

        result.runtime_config_path =
            result.package_root / runtime_config_path.lexically_normal();
    }

    return result;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
