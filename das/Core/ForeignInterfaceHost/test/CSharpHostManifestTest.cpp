// Copyright 2026.

#include <gtest/gtest.h>

#include <das/Core/ForeignInterfaceHost/CSharpManifest.h>
#include <das/Core/Utils/StringUtils.h>
#include <das/DasTypes.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

using namespace DAS::Core::ForeignInterfaceHost;

namespace
{
    struct CSharpManifestJsonOptions
    {
        bool             with_name = true;
        bool             with_plugin_filename_extension = true;
        bool             with_target_framework = true;
        bool             with_entry_point = true;
        bool             with_runtime_config = false;
        bool             runtime_config_is_null = false;
        bool             runtime_config_is_number = false;
        bool             name_is_number = false;
        bool             plugin_ext_is_number = false;
        bool             target_framework_is_number = false;
        bool             entry_point_is_number = false;
        std::string_view name = "DasCSharpTestPlugin";
        std::string_view plugin_ext = "dll";
        std::string_view target_framework = "net8.0";
        std::string_view entry_point =
            "Das.TestPlugin.TestPluginEntrypoint.Create";
        std::string_view runtime_config = "runtimeconfig.json";
    };

    std::string MakeCSharpManifestJson(
        const CSharpManifestJsonOptions& options = {})
    {
        std::string result =
            R"({"author":"DAS","version":"1.0","description":"test","supportedSystem":"Windows","language":"CSharp","guid":"35BF38D4-7760-42EA-8A9C-9F2BF7C3CBDA")";
        if (options.with_name)
        {
            if (options.name_is_number)
            {
                result += R"(,"name":)";
                result += std::string{options.name};
            }
            else
            {
                result += R"(,"name":")" + std::string{options.name} + R"(")";
            }
        }
        if (options.with_plugin_filename_extension)
        {
            if (options.plugin_ext_is_number)
            {
                result += R"(,"pluginFilenameExtension":)";
                result += std::string{options.plugin_ext};
            }
            else
            {
                result += R"(,"pluginFilenameExtension":")"
                          + std::string{options.plugin_ext} + R"(")";
            }
        }
        if (options.with_target_framework)
        {
            if (options.target_framework_is_number)
            {
                result += R"(,"targetFramework":)";
                result += std::string{options.target_framework};
            }
            else
            {
                result += R"(,"targetFramework":")"
                          + std::string{options.target_framework} + R"(")";
            }
        }
        if (options.with_entry_point)
        {
            if (options.entry_point_is_number)
            {
                result += R"(,"entryPoint":)";
                result += std::string{options.entry_point};
            }
            else
            {
                result += R"(,"entryPoint":")"
                          + std::string{options.entry_point} + R"(")";
            }
        }
        if (options.with_runtime_config)
        {
            if (options.runtime_config_is_null)
            {
                result += R"(,"runtimeConfigPath":null)";
            }
            else if (options.runtime_config_is_number)
            {
                result += R"(,"runtimeConfigPath":123)";
            }
            else
            {
                result += R"(,"runtimeConfigPath":")"
                          + std::string{options.runtime_config} + R"(")";
            }
        }
        result += "}";
        return result;
    }

    CSharpManifest ParseManifest(
        const std::filesystem::path& manifest_path,
        const std::string&           manifest_json)
    {
        auto manifest = ParseCSharpManifest(manifest_path, manifest_json);
        if (!manifest)
        {
            throw std::runtime_error(
                DAS::Utils::Format("Parse failed: {}", manifest.error()));
        }
        return *manifest;
    }

    DasResult ParseError(
        const std::filesystem::path& manifest_path,
        const std::string&           manifest_json)
    {
        auto manifest = ParseCSharpManifest(manifest_path, manifest_json);
        if (manifest)
        {
            return DAS_S_OK;
        }
        return manifest.error();
    }
} // namespace

TEST(CSharpHostManifest, CSharpManifestRequiresName)
{
    CSharpManifestJsonOptions options{};
    options.with_name = false;

    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(options)),
        DAS_E_CSHARP_MANIFEST_INVALID);
}

TEST(CSharpHostManifest, CSharpManifestRequiresStringName)
{
    CSharpManifestJsonOptions options{};
    options.name_is_number = true;
    options.name = "1";

    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(options)),
        DAS_E_CSHARP_MANIFEST_INVALID);
}

TEST(CSharpHostManifest, CSharpManifestRequiresPluginFilenameExtension)
{
    CSharpManifestJsonOptions options{};
    options.with_plugin_filename_extension = false;

    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(options)),
        DAS_E_CSHARP_MANIFEST_INVALID);
}

TEST(CSharpHostManifest, CSharpManifestRequiresStringPluginFilenameExtension)
{
    CSharpManifestJsonOptions options{};
    options.plugin_ext_is_number = true;
    options.plugin_ext = "1";

    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(options)),
        DAS_E_CSHARP_MANIFEST_INVALID);
}

TEST(
    CSharpHostManifest,
    CSharpManifestRejectsDotPrefixedPluginFilenameExtension)
{
    CSharpManifestJsonOptions options{};
    options.plugin_ext = ".dll";

    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(options)),
        DAS_E_CSHARP_MANIFEST_INVALID);
}

TEST(CSharpHostManifest, CSharpManifestRequiresTargetFramework)
{
    CSharpManifestJsonOptions options{};
    options.with_target_framework = false;

    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(options)),
        DAS_E_CSHARP_MANIFEST_INVALID);
}

TEST(CSharpHostManifest, CSharpManifestRequiresStringTargetFramework)
{
    CSharpManifestJsonOptions options{};
    options.target_framework_is_number = true;
    options.target_framework = "1";

    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(options)),
        DAS_E_CSHARP_MANIFEST_INVALID);
}

TEST(CSharpHostManifest, CSharpManifestRequiresEntryPoint)
{
    CSharpManifestJsonOptions options{};
    options.with_entry_point = false;

    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(options)),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
}

TEST(CSharpHostManifest, CSharpManifestRequiresStringEntryPoint)
{
    CSharpManifestJsonOptions options{};
    options.entry_point_is_number = true;
    options.entry_point = "2";

    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(options)),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
}

TEST(CSharpHostManifest, CSharpManifestRejectsNonStringRuntimeConfigPath)
{
    CSharpManifestJsonOptions options{};
    options.with_runtime_config = true;
    options.runtime_config_is_number = true;
    options.runtime_config = "2";

    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(options)),
        DAS_E_CSHARP_MANIFEST_INVALID);
}

TEST(CSharpHostManifest, CSharpTargetFrameworkClassification)
{
    EXPECT_EQ(
        ClassifyCSharpTargetFramework("net48").value(),
        CSharpTargetFrameworkFamily::NetFx);
    EXPECT_EQ(
        ClassifyCSharpTargetFramework("net4x").value(),
        CSharpTargetFrameworkFamily::NetFx);
    EXPECT_EQ(
        ClassifyCSharpTargetFramework(".NET Framework").value(),
        CSharpTargetFrameworkFamily::NetFx);
    EXPECT_EQ(
        ClassifyCSharpTargetFramework("NETFRAMEWORK").value(),
        CSharpTargetFrameworkFamily::NetFx);
    EXPECT_EQ(
        ClassifyCSharpTargetFramework("net6.0").value(),
        CSharpTargetFrameworkFamily::ModernDotNet);
}

TEST(CSharpHostManifest, CSharpTargetFrameworkRejectsUnsupportedValue)
{
    const auto result = ClassifyCSharpTargetFramework("netstandard2.0");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), DAS_E_CSHARP_UNSUPPORTED_TFM);
}

TEST(CSharpHostManifest, CSharpTargetFrameworkDoesNotDependOnRuntimeConfigPath)
{
    CSharpManifestJsonOptions options{};
    options.target_framework = "net8.0";
    options.with_runtime_config = false;

    const auto manifest = ParseManifest(
        "package/DasCSharpTestPlugin.json",
        MakeCSharpManifestJson(options));

    EXPECT_EQ(
        manifest.target_framework_family,
        CSharpTargetFrameworkFamily::ModernDotNet);
}

TEST(CSharpHostManifest, CSharpRuntimeConfigDefaultsForModernFramework)
{
    const auto manifest = ParseManifest(
        "package/DasCSharpTestPlugin.json",
        MakeCSharpManifestJson());

    ASSERT_TRUE(manifest.runtime_config_path);
    EXPECT_EQ(
        *manifest.runtime_config_path,
        std::filesystem::path{"package"}
            / "DasCSharpTestPlugin.runtimeconfig.json");
}

TEST(CSharpHostManifest, CSharpRuntimeConfigPathUsesPackageRelativeValue)
{
    CSharpManifestJsonOptions options{};
    options.with_runtime_config = true;
    options.runtime_config = "runtime/DasCSharpTestPlugin.runtimeconfig.json";

    const auto manifest = ParseManifest(
        "package/DasCSharpTestPlugin.json",
        MakeCSharpManifestJson(options));

    ASSERT_TRUE(manifest.runtime_config_path);
    EXPECT_EQ(
        *manifest.runtime_config_path,
        std::filesystem::path{"package"}
            / "runtime/DasCSharpTestPlugin.runtimeconfig.json");
}

TEST(CSharpHostManifest, CSharpRuntimeConfigPathRejectsUnsafeInput)
{
    CSharpManifestJsonOptions absolute{};
    absolute.with_runtime_config = true;
    absolute.runtime_config = "/tmp/host.runtimeconfig.json";
    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(absolute)),
        DAS_E_CSHARP_MANIFEST_INVALID);

    CSharpManifestJsonOptions parent{};
    parent.with_runtime_config = true;
    parent.runtime_config = "../host.runtimeconfig.json";
    EXPECT_EQ(
        ParseError(
            "package/DasCSharpTestPlugin.json",
            MakeCSharpManifestJson(parent)),
        DAS_E_CSHARP_MANIFEST_INVALID);
}

TEST(CSharpHostManifest, CSharpRuntimeConfigPathIgnoredForNetFx)
{
    CSharpManifestJsonOptions options{};
    options.target_framework = "net48";
    options.with_runtime_config = true;
    options.runtime_config = "host.runtimeconfig.json";

    const auto manifest = ParseManifest(
        "package/DasCSharpTestPlugin.json",
        MakeCSharpManifestJson(options));

    EXPECT_FALSE(manifest.runtime_config_path);
}

TEST(CSharpHostManifest, CSharpEntryPointSplitUsesLastDot)
{
    const auto entry_point =
        SplitCSharpEntryPoint("Das.TestPlugin.Outer.Entry.Create").value();
    EXPECT_EQ(entry_point.type_name, "Das.TestPlugin.Outer.Entry");
    EXPECT_EQ(entry_point.method_name, "Create");
}

TEST(
    CSharpHostManifest,
    CSharpEntryPointRejectsMethodOnlyAndNestedOrGenericForms)
{
    EXPECT_EQ(
        SplitCSharpEntryPoint("Create").error(),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
    EXPECT_EQ(
        SplitCSharpEntryPoint("Das.TestPlugin.").error(),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
    EXPECT_EQ(
        SplitCSharpEntryPoint(".Create").error(),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
    EXPECT_EQ(
        SplitCSharpEntryPoint("Das.TestPlugin+Inner.Create").error(),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
    EXPECT_EQ(
        SplitCSharpEntryPoint("Das.TestPlugin.EntryFactory`1.Create").error(),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
    EXPECT_EQ(
        SplitCSharpEntryPoint("Das.TestPlugin.Entry.Create`1").error(),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
}

TEST(CSharpHostManifest, CSharpEntryPointRejectsScanBasedDiscoveryForms)
{
    EXPECT_EQ(
        SplitCSharpEntryPoint("Das.TestPlugin.EntryPoint.*").error(),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
    EXPECT_EQ(
        SplitCSharpEntryPoint("Das.TestPlugin.EntryFactory.Create()").error(),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
    EXPECT_EQ(
        SplitCSharpEntryPoint("Das.TestPlugin.EntryFactory:Create").error(),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
}

TEST(CSharpHostManifest, CSharpAssemblyPathFollowsPackageConvention)
{
    const auto manifest = ParseManifest(
        "package/DasCSharpTestPlugin.json",
        MakeCSharpManifestJson());

    EXPECT_EQ(
        manifest.plugin_binary_path,
        std::filesystem::path{"package"} / "DasCSharpTestPlugin.dll");
}
