#include <cstring>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    std::string BasicPluginJsonWith(std::string_view extra_fields)
    {
        std::string result = R"(
    {
        "name": "TestPlugin",
        "author": "test",
        "version": "1.0",
        "guid": "35BF38D4-7760-42EA-8A9C-9F2BF7C3CBDA",
        "description": "test",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": []
    )";

        if (!extra_fields.empty())
        {
            result += ",";
            result += extra_fields;
        }

        result += "\n    }";
        return result;
    }

    template <class T>
    T JsonToStruct(const std::string& string)
    {
        const auto json_opt = Das::Utils::ParseYyjsonFromString(string);
        if (!json_opt)
        {
            throw std::runtime_error("Failed to parse JSON");
        }
        T result{};
        if constexpr (
            std::is_same_v<
                T,
                DAS::Core::ForeignInterfaceHost::PluginPackageDesc>)
        {
            DAS::Core::ForeignInterfaceHost::ParsePluginPackageDescFromJson(
                json_opt->as_object().value(),
                result);
        }
        return result;
    }
} // anonymous namespace

TEST(PluginPackageDescTest, FromBasicJson)
{
    constexpr auto test_string = R"(
    {
        "name": "test_name",
        "author": "test_author",
        "version": "test_version",
        "guid" : "35BF38D4-7760-42EA-8A9C-9F2BF7C3CBDA",
        "description": "test_description",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": []
    }
    )";

    const auto plugin_desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);

    EXPECT_EQ(plugin_desc.name, "test_name");
    EXPECT_EQ(plugin_desc.author, "test_author");
    EXPECT_EQ(plugin_desc.version, "test_version");
    const auto guid = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
        "35BF38D4-7760-42EA-8A9C-9F2BF7C3CBDA");
    EXPECT_EQ(plugin_desc.supported_system, "Windows");
    EXPECT_EQ(
        plugin_desc.language,
        DAS::Core::ForeignInterfaceHost::ForeignInterfaceLanguage::Cpp);
    EXPECT_EQ(plugin_desc.plugin_filename_extension, "dll");
    EXPECT_EQ(plugin_desc.guid, guid);
}

TEST(PluginPackageDescTest, LoadMode_MissingDefaultsToInProcess)
{
    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            BasicPluginJsonWith(""));

    EXPECT_EQ(
        desc.load_mode,
        DAS::Core::ForeignInterfaceHost::LoadMode::InProcess);
}

TEST(PluginPackageDescTest, LoadMode_NumericValues)
{
    const auto in_process =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            BasicPluginJsonWith(R"("loadMode": 0)"));
    EXPECT_EQ(
        in_process.load_mode,
        DAS::Core::ForeignInterfaceHost::LoadMode::InProcess);

    const auto ipc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            BasicPluginJsonWith(R"("loadMode": 1)"));
    EXPECT_EQ(ipc.load_mode, DAS::Core::ForeignInterfaceHost::LoadMode::Ipc);
}

TEST(PluginPackageDescTest, LoadMode_StringValues)
{
    using DAS::Core::ForeignInterfaceHost::LoadMode;

    const std::vector<std::pair<std::string, LoadMode>> cases{
        {"ipc", LoadMode::Ipc},
        {"IPC", LoadMode::Ipc},
        {"inProcess", LoadMode::InProcess},
        {"InProcess", LoadMode::InProcess},
        {"in_process", LoadMode::InProcess},
    };

    for (const auto& [value, expected] : cases)
    {
        const auto desc =
            JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
                BasicPluginJsonWith(R"("loadMode": ")" + value + R"(")"));
        EXPECT_EQ(desc.load_mode, expected) << "loadMode=" << value;
    }
}

TEST(PluginPackageDescTest, LoadMode_InvalidStringFails)
{
    try
    {
        (void)JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            BasicPluginJsonWith(R"("loadMode": "sidecar")"));
        FAIL() << "Invalid loadMode string should fail";
    }
    catch (const std::runtime_error& e)
    {
        EXPECT_NE(std::string{e.what()}.find("loadMode"), std::string::npos);
    }
}

TEST(PluginPackageDescTest, FromUnexpectedGuidJson)
{
    constexpr auto test_string = R"(
{
        "name": "test_name",
        "author": "test_author",
        "version": "test_version",
        "guid" : "ufuoiajoighoa",
        "description": "test_description",
        "language" : "CSharp",
        "supportedSystem": "Any",
        "pluginFilenameExtension": "dll",
        "settings": []
}
    )";

    EXPECT_THROW(
        {
            const auto plugin_desc = JsonToStruct<
                DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
                test_string);
        },
        DAS::Core::InvalidGuidStringSizeException);
}

TEST(PluginSettingsDescTest, FromBasicJson)
{
    constexpr auto test_string = R"(
    {
        "name": "test_name",
        "author": "test_author",
        "version": "test_version",
        "guid" : "35BF38D4-7760-42EA-8A9C-9F2BF7C3CBDA",
        "description": "test_description",
        "language" : "Python",
        "supportedSystem": "Linux",
        "pluginFilenameExtension": "py",
        "settings": [{
            "name": "test_setting_name",
            "type": "int",
            "defaultValue": 1,
            "description": "test_setting_description"
        }]
    }
    )";

    auto plugin_desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);
    EXPECT_EQ(plugin_desc.name, "test_name");
    EXPECT_EQ(plugin_desc.author, "test_author");
    EXPECT_EQ(plugin_desc.version, "test_version");
    const auto guid = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
        "35BF38D4-7760-42EA-8A9C-9F2BF7C3CBDA");
    EXPECT_EQ(plugin_desc.guid, guid);
    EXPECT_EQ(
        plugin_desc.language,
        DAS::Core::ForeignInterfaceHost::ForeignInterfaceLanguage::Python);
    EXPECT_EQ(plugin_desc.supported_system, "Linux");
    EXPECT_EQ(plugin_desc.plugin_filename_extension, "py");

    const auto& setting_desc = plugin_desc.settings_desc[0];

    EXPECT_EQ(setting_desc.name, "test_setting_name");
    EXPECT_EQ(setting_desc.type, Das::ExportInterface::DAS_TYPE_INT);
    EXPECT_EQ(std::get<std::int64_t>(setting_desc.default_value), 1);
    EXPECT_EQ(setting_desc.description, "test_setting_description");
}

TEST(PluginPackageDescTest, SettingsGroupPluginGuidKeyed)
{
    constexpr auto test_string = R"(
    {
        "name": "DasAdbAutomation",
        "author": "Dusk",
        "version": "0.1.0",
        "guid": "0527CD9E-1F26-44FB-BE5F-D63C5A11B754",
        "description": "ADB automation plugin package",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": {
            "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E": {
                "name": "adbRuntime",
                "description": "Shared ADB runtime plugin settings",
                "descriptors": [
                    {
                        "name": "adbPath",
                        "type": 4,
                        "defaultValue": "adb",
                        "description": "ADB executable path",
                        "required": true
                    },
                    {
                        "name": "deviceSerial",
                        "type": 4,
                        "defaultValue": "",
                        "description": "Optional target device serial",
                        "required": false
                    }
                ]
            },
            "8F08935F-11B9-4D3A-BB0D-96D1862FE3F6": {
                "name": "dailyAutomation",
                "description": "Daily automation plugin settings",
                "descriptors": [
                    {
                        "name": "logLevel",
                        "type": 4,
                        "defaultValue": "info",
                        "description": "Plugin log level",
                        "required": false,
                        "deprecationMessage": "Use verboseLogging instead"
                    }
                ]
            }
        }
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);

    ASSERT_EQ(desc.settings_groups.size(), 2u);

    const auto guid1 = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
        "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E");
    const auto it1 = desc.settings_groups.find(guid1);
    ASSERT_NE(it1, desc.settings_groups.end());
    EXPECT_EQ(it1->second.name, "adbRuntime");
    EXPECT_EQ(it1->second.description, "Shared ADB runtime plugin settings");
    ASSERT_EQ(it1->second.descriptors.size(), 2u);
    EXPECT_EQ(it1->second.descriptors[0].name, "adbPath");
    EXPECT_EQ(
        it1->second.descriptors[0].type,
        Das::ExportInterface::DAS_TYPE_STRING);
    EXPECT_EQ(
        std::get<std::string>(it1->second.descriptors[0].default_value),
        "adb");
    EXPECT_TRUE(it1->second.descriptors[0].required);
    EXPECT_FALSE(it1->second.descriptors[1].required);

    const auto guid2 = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
        "8F08935F-11B9-4D3A-BB0D-96D1862FE3F6");
    const auto it2 = desc.settings_groups.find(guid2);
    ASSERT_NE(it2, desc.settings_groups.end());
    ASSERT_EQ(it2->second.descriptors.size(), 1u);
    EXPECT_TRUE(it2->second.descriptors[0].deprecation_message.has_value());
    EXPECT_EQ(
        it2->second.descriptors[0].deprecation_message.value(),
        "Use verboseLogging instead");
}

TEST(PluginPackageDescTest, TaskDescriptorsKeyedByGuid)
{
    constexpr auto test_string = R"(
    {
        "name": "DasAdbAutomation",
        "author": "Dusk",
        "version": "0.1.0",
        "guid": "0527CD9E-1F26-44FB-BE5F-D63C5A11B754",
        "description": "ADB automation plugin package",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": {},
        "tasks": {
            "B4F60C54-67DF-407A-B891-6D3C90CDB9A1": {
                "pluginGuid": "8F08935F-11B9-4D3A-BB0D-96D1862FE3F6",
                "name": "dailyLogin",
                "description": "Open the game and perform daily login actions",
                "gameName": "Blue Archive",
                "descriptors": [
                    {
                        "name": "claimMail",
                        "type": 8,
                        "defaultValue": true,
                        "description": "Whether to claim mailbox rewards",
                        "required": false
                    },
                    {
                        "name": "maxRetryCount",
                        "type": 0,
                        "defaultValue": 3,
                        "description": "Maximum retry count for transient failures",
                        "required": false
                    }
                ]
            },
            "F0D46830-6A0E-42EC-85F2-A8E2C71D06A8": {
                "pluginGuid": "8F08935F-11B9-4D3A-BB0D-96D1862FE3F6",
                "name": "preLaunchHook",
                "description": "Run custom actions before another scheduled task",
                "gameName": "Blue Archive",
                "descriptors": [
                    {
                        "name": "scriptName",
                        "type": 4,
                        "defaultValue": "",
                        "description": "Name of the pre-task script to run",
                        "required": true
                    }
                ]
            }
        }
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);

    ASSERT_EQ(desc.task_descriptors.size(), 2u);

    const auto task_guid1 = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
        "B4F60C54-67DF-407A-B891-6D3C90CDB9A1");
    const auto it1 = desc.task_descriptors.find(task_guid1);
    ASSERT_NE(it1, desc.task_descriptors.end());
    EXPECT_EQ(it1->second.name, "dailyLogin");
    EXPECT_EQ(
        it1->second.description,
        "Open the game and perform daily login actions");
    EXPECT_EQ(it1->second.game_name.value(), "Blue Archive");
    const auto expected_plugin_guid =
        DAS::Core::ForeignInterfaceHost::MakeDasGuid(
            "8F08935F-11B9-4D3A-BB0D-96D1862FE3F6");
    EXPECT_EQ(it1->second.plugin_guid, expected_plugin_guid);
    ASSERT_EQ(it1->second.descriptors.size(), 2u);
    EXPECT_EQ(it1->second.descriptors[0].name, "claimMail");
    EXPECT_EQ(
        it1->second.descriptors[0].type,
        Das::ExportInterface::DAS_TYPE_BOOL);
    EXPECT_TRUE(std::get<bool>(it1->second.descriptors[0].default_value));
    EXPECT_FALSE(it1->second.descriptors[0].required);
    EXPECT_EQ(it1->second.descriptors[1].name, "maxRetryCount");
    EXPECT_EQ(
        it1->second.descriptors[1].type,
        Das::ExportInterface::DAS_TYPE_INT);
    EXPECT_EQ(
        std::get<std::int64_t>(it1->second.descriptors[1].default_value),
        3);

    const auto task_guid2 = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
        "F0D46830-6A0E-42EC-85F2-A8E2C71D06A8");
    const auto it2 = desc.task_descriptors.find(task_guid2);
    ASSERT_NE(it2, desc.task_descriptors.end());
    EXPECT_EQ(it2->second.name, "preLaunchHook");
    ASSERT_EQ(it2->second.descriptors.size(), 1u);
    EXPECT_EQ(it2->second.descriptors[0].name, "scriptName");
    EXPECT_TRUE(it2->second.descriptors[0].required);
}

TEST(PluginPackageDescTest, TaskDescriptorAuthoringCapability)
{
    constexpr auto test_string = R"(
    {
        "name": "AuthoringPlugin",
        "author": "Dusk",
        "version": "1.0.0",
        "guid": "0527CD9E-1F26-44FB-BE5F-D63C5A11B754",
        "description": "Plugin with authoring capability",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": [],
        "tasks": {
            "B4F60C54-67DF-407A-B891-6D3C90CDB9A1": {
                "pluginGuid": "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E",
                "name": "dailyLogin",
                "description": "Daily login task",
                "authoring": {
                    "featureIndex": 2,
                    "supportedKinds": ["formSequence", "graph"]
                }
            }
        }
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);
    const auto task_guid = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
        "B4F60C54-67DF-407A-B891-6D3C90CDB9A1");

    const auto it = desc.task_descriptors.find(task_guid);
    ASSERT_NE(it, desc.task_descriptors.end());
    ASSERT_TRUE(it->second.authoring.has_value());
    EXPECT_EQ(it->second.authoring->feature_index, 2u);
    ASSERT_EQ(it->second.authoring->supported_kinds.size(), 2u);
    EXPECT_EQ(it->second.authoring->supported_kinds[0], "formSequence");
    EXPECT_EQ(it->second.authoring->supported_kinds[1], "graph");
}

TEST(PluginPackageDescTest, TaskDescriptorComponentCapabilities)
{
    constexpr auto test_string = R"(
    {
        "name": "ComponentPlugin",
        "author": "Dusk",
        "version": "1.0.0",
        "guid": "0527CD9E-1F26-44FB-BE5F-D63C5A11B754",
        "description": "Plugin with component capabilities",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": [],
        "tasks": {
            "B4F60C54-67DF-407A-B891-6D3C90CDB9A1": {
                "pluginGuid": "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E",
                "name": "dailyLogin",
                "description": "Daily login task",
                "components": [
                    {
                        "componentGuid": "78B71988-2125-4A2B-8B78-02A804570101",
                        "factoryFeatureIndex": 3,
                        "role": "flowControl"
                    }
                ]
            }
        }
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);
    const auto task_guid = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
        "B4F60C54-67DF-407A-B891-6D3C90CDB9A1");
    const auto component_guid = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
        "78B71988-2125-4A2B-8B78-02A804570101");

    const auto it = desc.task_descriptors.find(task_guid);
    ASSERT_NE(it, desc.task_descriptors.end());
    ASSERT_EQ(it->second.components.size(), 1u);
    EXPECT_EQ(it->second.components[0].component_guid, component_guid);
    EXPECT_EQ(it->second.components[0].factory_feature_index, 3u);
    EXPECT_EQ(it->second.components[0].role, "flowControl");
}

TEST(PluginPackageDescTest, ResourcePath_ExplicitValue)
{
    constexpr auto test_string = R"(
    {
        "name": "TestPlugin",
        "author": "test",
        "version": "1.0",
        "guid": "35BF38D4-7760-42EA-8A9C-9F2BF7C3CBDA",
        "description": "test",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "resourcePath": "assets/images",
        "settings": []
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);

    ASSERT_TRUE(desc.opt_resource_path.has_value());
    EXPECT_EQ(desc.opt_resource_path.value(), "assets/images");
}

TEST(PluginPackageDescTest, ResourcePath_DefaultValue)
{
    constexpr auto test_string = R"(
    {
        "name": "TestPlugin",
        "author": "test",
        "version": "1.0",
        "guid": "35BF38D4-7760-42EA-8A9C-9F2BF7C3CBDA",
        "description": "test",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": []
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);

    ASSERT_TRUE(desc.opt_resource_path.has_value());
    EXPECT_EQ(desc.opt_resource_path.value(), "resource");
}

TEST(PluginPackageDescTest, ResourcePath_FolderModeOnlySemantics)
{
    // resourcePath is parsed from JSON regardless of manifest layout.
    // However, the resource lookup semantics (PluginResourceIndex) only
    // apply to folder-mode packages. Flat-file manifest mode is not
    // supported for resource loading per Phase 51 D-03.
    constexpr auto test_string = R"(
    {
        "name": "TestPlugin",
        "author": "test",
        "version": "1.0",
        "guid": "35BF38D4-7760-42EA-8A9C-9F2BF7C3CBDA",
        "description": "test",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "resourcePath": "res",
        "settings": []
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);

    ASSERT_TRUE(desc.opt_resource_path.has_value());
    EXPECT_EQ(desc.opt_resource_path.value(), "res");
    // Note: folder-mode enforcement happens at scan time in
    // PluginResourceIndex::ScanAndPublish, not at parser level.
}

TEST(PluginPackageDescTest, SettingsAndTasksTogether)
{
    constexpr auto test_string = R"(
    {
        "name": "FullPlugin",
        "author": "Dusk",
        "version": "1.0.0",
        "guid": "0527CD9E-1F26-44FB-BE5F-D63C5A11B754",
        "description": "Plugin with both settings groups and task descriptors",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": {
            "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E": {
                "name": "runtime",
                "description": "Runtime settings",
                "descriptors": [
                    {
                        "name": "verbose",
                        "type": 8,
                        "defaultValue": false,
                        "description": "Enable verbose logging"
                    }
                ]
            }
        },
        "tasks": {
            "B4F60C54-67DF-407A-B891-6D3C90CDB9A1": {
                "pluginGuid": "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E",
                "name": "dailyLogin",
                "description": "Daily login task",
                "gameName": "TestGame",
                "descriptors": [
                    {
                        "name": "retryCount",
                        "type": 0,
                        "defaultValue": 3,
                        "description": "Retry count"
                    }
                ]
            }
        }
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);

    ASSERT_EQ(desc.settings_groups.size(), 1u);
    ASSERT_EQ(desc.task_descriptors.size(), 1u);

    const auto settings_guid = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
        "65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E");
    const auto it_s = desc.settings_groups.find(settings_guid);
    ASSERT_NE(it_s, desc.settings_groups.end());
    EXPECT_EQ(it_s->second.name, "runtime");

    const auto task_guid = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
        "B4F60C54-67DF-407A-B891-6D3C90CDB9A1");
    const auto it_t = desc.task_descriptors.find(task_guid);
    ASSERT_NE(it_t, desc.task_descriptors.end());
    EXPECT_EQ(it_t->second.name, "dailyLogin");
    EXPECT_EQ(it_t->second.game_name.value(), "TestGame");
}
