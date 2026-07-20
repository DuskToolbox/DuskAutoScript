#include <cstring>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr std::string_view kTaskComponentFactoryGuid =
        "0196057D-1029-4D9D-A2D0-81869F40F721";
    constexpr std::string_view kAlternateTaskComponentFactoryGuid =
        "1196057D-1029-4D9D-A2D0-81869F40F722";
    constexpr std::string_view kTaskComponentGuid =
        "78B71988-2125-4A2B-8B78-02A804570101";
    constexpr std::string_view kAlternateTaskComponentGuid =
        "88B71988-2125-4A2B-8B78-02A804570102";
    constexpr std::string_view kTaskGuid =
        "B4F60C54-67DF-407A-B891-6D3C90CDB9A1";

    template <class T>
    concept HasStableNameField = requires(T value) { value.stable_name; };

    template <class T>
    concept HasDebugNameField = requires(T value) { value.debug_name; };

    template <class T>
    concept HasRoleField = requires(T value) { value.role; };

    template <class T>
    concept HasDefinitionPathField =
        requires(T value) { value.definition_path; };

    template <class T>
    concept HasComponentsField = requires(T value) { value.components; };

    template <class T>
    concept HasExecutionComponentField =
        requires(T value) { value.execution_component; };

    static_assert(
        !HasStableNameField<
            DAS::Core::ForeignInterfaceHost::TaskComponentManifestEntryDesc>);
    static_assert(
        !HasDebugNameField<
            DAS::Core::ForeignInterfaceHost::TaskComponentManifestEntryDesc>);
    static_assert(
        !HasRoleField<
            DAS::Core::ForeignInterfaceHost::TaskComponentManifestEntryDesc>);
    static_assert(
        !HasDefinitionPathField<
            DAS::Core::ForeignInterfaceHost::TaskComponentManifestEntryDesc>);
    static_assert(
        !HasComponentsField<DAS::Core::ForeignInterfaceHost::TaskDescriptor>);
    static_assert(HasExecutionComponentField<
                  DAS::Core::ForeignInterfaceHost::TaskDescriptor>);
    static_assert(HasComponentsField<
                  DAS::Core::ForeignInterfaceHost::TaskComponentsManifestDesc>);

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

    std::string BasicPluginJsonWithLanguage(std::string_view language)
    {
        return std::string{R"(
    {
        "name": "TestPlugin",
        "author": "test",
        "version": "1.0",
        "guid": "35BF38D4-7760-42EA-8A9C-9F2BF7C3CBDA",
        "description": "test",
        "supportedSystem": "Windows",
        "language": ")"}
               + std::string{language} + R"(",
        "pluginFilenameExtension": "dll",
        "settings": []
    }
    )";
    }

    template <class T>
    T JsonToStruct(
        const std::string& string,
        yyjson::ReadFlag   flags = yyjson::ReadFlag::NoFlag)
    {
        const auto json_opt = Das::Utils::ParseYyjsonFromString(string, flags);
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

    std::string ValidDefinitionFor(std::string_view component_guid)
    {
        return std::string{R"({"schemaVersion":1,"componentGuid":")"}
               + std::string{component_guid}
               + R"(","kind":"branch","inputs":[],"outputs":[],"config":{},"diagnostics":[]})";
    }

    std::string EntryWithFactoryAndDefinition(
        std::string_view factory_guid,
        std::string_view definition)
    {
        return std::string{R"({"factoryGuid":")"} + std::string{factory_guid}
               + R"(","definition":)" + std::string{definition} + "}";
    }

    std::string TaskComponentsWithEntry(
        std::string_view component_key,
        std::string_view entry)
    {
        return std::string{R"({"factories":[")"}
               + std::string{kTaskComponentFactoryGuid}
               + R"("],"components":{")" + std::string{component_key} + R"(":)"
               + std::string{entry} + "}}";
    }

    std::string TaskEntryWithExecutionComponent(std::string_view component_guid)
    {
        return std::string{
                   R"({"pluginGuid":"65EDE9D8-09A8-4F01-B4AF-6614C5BA1C8E",)"
                   R"("name":"repositoryBackedTask",)"
                   R"("description":"Repository-backed task",)"
                   R"("executionComponent":{"componentGuid":")"}
               + std::string{component_guid} + R"("}})";
    }

    std::string MinimalExecutionComponentPluginJson(
        std::string_view task_component_guid,
        std::string_view task_entry)
    {
        const auto component_entry = EntryWithFactoryAndDefinition(
            kTaskComponentFactoryGuid,
            ValidDefinitionFor(task_component_guid));
        return BasicPluginJsonWith(
            std::string{R"("taskComponents":)"}
            + TaskComponentsWithEntry(task_component_guid, component_entry)
            + R"(,"tasks":{")" + std::string{kTaskGuid} + R"(":)"
            + std::string{task_entry} + "}");
    }

    void ExpectInvalidTaskComponents(
        std::string_view task_components_json,
        std::string_view expected_reason)
    {
        const auto manifest = BasicPluginJsonWith(
            std::string{R"("taskComponents":)"}
            + std::string{task_components_json});

        try
        {
            (void)JsonToStruct<
                DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(manifest);
            FAIL() << "Invalid taskComponents should fail";
        }
        catch (const std::runtime_error& e)
        {
            EXPECT_NE(
                std::string{e.what()}.find(expected_reason),
                std::string::npos)
                << e.what();
        }
    }

    void ExpectInvalidTaskDescriptor(
        std::string_view manifest,
        std::string_view expected_reason)
    {
        try
        {
            (void)JsonToStruct<
                DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
                std::string{manifest});
            FAIL() << "Invalid task descriptor should fail";
        }
        catch (const std::exception& e)
        {
            EXPECT_NE(
                std::string{e.what()}.find(expected_reason),
                std::string::npos)
                << e.what();
        }
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

TEST(PluginPackageDescTest, Language_ExactNodeString)
{
    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            BasicPluginJsonWithLanguage("Node"));

    EXPECT_EQ(
        desc.language,
        DAS::Core::ForeignInterfaceHost::ForeignInterfaceLanguage::Node);
}

TEST(PluginPackageDescTest, Language_LowercaseNodeFails)
{
    try
    {
        (void)JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            BasicPluginJsonWithLanguage("node"));
        FAIL() << "Lowercase node language should fail";
    }
    catch (const std::runtime_error& e)
    {
        EXPECT_NE(std::string{e.what()}.find("language"), std::string::npos)
            << e.what();
    }
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
                    "factoryGuid": "0196057D-1029-4D9D-A2D0-81869F40F721",
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
    EXPECT_EQ(
        it->second.authoring->factory_guid,
        DAS::Core::ForeignInterfaceHost::MakeDasGuid(
            "0196057D-1029-4D9D-A2D0-81869F40F721"));
    ASSERT_EQ(it->second.authoring->supported_kinds.size(), 2u);
    EXPECT_EQ(it->second.authoring->supported_kinds[0], "formSequence");
    EXPECT_EQ(it->second.authoring->supported_kinds[1], "graph");
}

TEST(PluginDescTaskExecutionComponentTest, ParsesExecutionComponentGuid)
{
    const auto manifest = MinimalExecutionComponentPluginJson(
        kTaskComponentGuid,
        TaskEntryWithExecutionComponent(kTaskComponentGuid));

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            manifest);
    const auto task_guid =
        DAS::Core::ForeignInterfaceHost::MakeDasGuid(std::string{kTaskGuid});

    const auto it = desc.task_descriptors.find(task_guid);
    ASSERT_NE(it, desc.task_descriptors.end());
    ASSERT_TRUE(it->second.execution_component.has_value());
    EXPECT_EQ(
        it->second.execution_component->component_guid,
        DAS::Core::ForeignInterfaceHost::MakeDasGuid(
            std::string{kTaskComponentGuid}));
}

TEST(PluginDescTaskExecutionComponentTest, RejectsInvalidComponentGuid)
{
    const auto manifest = MinimalExecutionComponentPluginJson(
        kTaskComponentGuid,
        TaskEntryWithExecutionComponent("not-a-guid"));

    ExpectInvalidTaskDescriptor(manifest, "executionComponent.componentGuid");
}

TEST(PluginDescTaskExecutionComponentTest, RejectsMissingDeclaredComponentGuid)
{
    const auto manifest = MinimalExecutionComponentPluginJson(
        kTaskComponentGuid,
        TaskEntryWithExecutionComponent(kAlternateTaskComponentGuid));

    ExpectInvalidTaskDescriptor(manifest, "executionComponent.componentGuid");
}

TEST(
    PluginDescTaskExecutionComponentTest,
    RejectsMissingTaskComponentsForExecutionComponent)
{
    const auto manifest = BasicPluginJsonWith(
        R"("tasks":{")" + std::string{kTaskGuid} + R"(":)"
        + TaskEntryWithExecutionComponent(kTaskComponentGuid) + "}");

    ExpectInvalidTaskDescriptor(manifest, "executionComponent.componentGuid");
}

TEST(PluginDescTaskComponentsTest, ParsesTopLevelManifest)
{
    constexpr auto test_string = R"(
    {
        "name": "TaskComponentPlugin",
        "author": "Dusk",
        "version": "1.0.0",
        "guid": "0527CD9E-1F26-44FB-BE5F-D63C5A11B754",
        "description": "Plugin with task component manifest",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": [],
        "taskComponents": {
            "factories": [
                "0196057D-1029-4D9D-A2D0-81869F40F721"
            ],
            "components": {
                "78B71988-2125-4A2B-8B78-02A804570101": {
                    "factoryGuid": "0196057D-1029-4D9D-A2D0-81869F40F721",
                    "definition": {
                        "schemaVersion": 1,
                        "componentGuid": "78B71988-2125-4A2B-8B78-02A804570101",
                        "kind": "branch",
                        "inputs": [],
                        "outputs": [],
                        "config": {},
                        "diagnostics": [],
                        "thirdPartyDefinitionMetadata": true
                    },
                    "thirdPartyComponentMetadata": "accepted"
                }
            },
            "thirdPartyManifestMetadata": 42
        }
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);

    ASSERT_TRUE(desc.task_components.has_value());
    ASSERT_TRUE(desc.task_components->factories.has_value());
    ASSERT_EQ(desc.task_components->factories->size(), 1u);
    EXPECT_EQ(
        (*desc.task_components->factories)[0],
        "0196057D-1029-4D9D-A2D0-81869F40F721");

    ASSERT_TRUE(desc.task_components->components.has_value());
    const auto component_it = desc.task_components->components->find(
        "78B71988-2125-4A2B-8B78-02A804570101");
    ASSERT_NE(component_it, desc.task_components->components->end());
    EXPECT_EQ(
        component_it->second.factory_guid.value(),
        "0196057D-1029-4D9D-A2D0-81869F40F721");
    ASSERT_TRUE(component_it->second.definition.has_value());
    const auto definition_obj = component_it->second.definition->as_object();
    ASSERT_TRUE(definition_obj.has_value());
    EXPECT_TRUE(definition_obj->contains(
        std::string_view("thirdPartyDefinitionMetadata")));
}

TEST(PluginDescTaskComponentsTest, RejectsInvalidFactoryGuid)
{
    ExpectInvalidTaskComponents(
        R"({"factories":["not-a-guid"],"components":{}})",
        "taskComponents.factories[0]: invalid factory GUID");
}

TEST(PluginDescTaskComponentsTest, RejectsInvalidComponentGuidKey)
{
    const auto entry = EntryWithFactoryAndDefinition(
        kTaskComponentFactoryGuid,
        ValidDefinitionFor(kTaskComponentGuid));

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry("not-a-guid", entry),
        "taskComponents.components.not-a-guid: invalid component GUID key");
}

TEST(PluginDescTaskComponentsTest, RejectsMissingFactoryGuid)
{
    const auto entry = std::string{R"({"definition":)"}
                       + ValidDefinitionFor(kTaskComponentGuid) + "}";

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry(kTaskComponentGuid, entry),
        "taskComponents.components.78B71988-2125-4A2B-8B78-02A804570101."
        "factoryGuid: missing required string");
}

TEST(PluginDescTaskComponentsTest, RejectsUndeclaredFactoryGuid)
{
    const auto entry = EntryWithFactoryAndDefinition(
        kAlternateTaskComponentFactoryGuid,
        ValidDefinitionFor(kTaskComponentGuid));

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry(kTaskComponentGuid, entry),
        "taskComponents.components.78B71988-2125-4A2B-8B78-02A804570101."
        "factoryGuid: undeclared factory GUID");
}

TEST(PluginDescTaskComponentsTest, RejectsMissingDefinition)
{
    const auto entry = std::string{R"({"factoryGuid":")"}
                       + std::string{kTaskComponentFactoryGuid} + R"("})";

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry(kTaskComponentGuid, entry),
        "taskComponents.components.78B71988-2125-4A2B-8B78-02A804570101."
        "definition: missing required object");
}

TEST(PluginDescTaskComponentsTest, RejectsNonObjectDefinition)
{
    const auto entry =
        EntryWithFactoryAndDefinition(kTaskComponentFactoryGuid, R"([])");

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry(kTaskComponentGuid, entry),
        "taskComponents.components.78B71988-2125-4A2B-8B78-02A804570101."
        "definition: expected object");
}

TEST(PluginDescTaskComponentsTest, RejectsMismatchedDefinitionComponentGuid)
{
    const auto entry = EntryWithFactoryAndDefinition(
        kTaskComponentFactoryGuid,
        ValidDefinitionFor(kAlternateTaskComponentGuid));

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry(kTaskComponentGuid, entry),
        "taskComponents.components.78B71988-2125-4A2B-8B78-02A804570101."
        "definition.componentGuid: must match component key");
}

TEST(PluginDescTaskComponentsTest, RejectsMissingDefinitionSchemaVersion)
{
    const auto definition = std::string{
        R"({"componentGuid":"78B71988-2125-4A2B-8B78-02A804570101",)"
        R"("kind":"branch","inputs":[],"outputs":[],"config":{},"diagnostics":[]})"};
    const auto entry =
        EntryWithFactoryAndDefinition(kTaskComponentFactoryGuid, definition);

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry(kTaskComponentGuid, entry),
        "taskComponents.components.78B71988-2125-4A2B-8B78-02A804570101."
        "definition.schemaVersion: missing required field");
}

TEST(PluginDescTaskComponentsTest, RejectsMissingDefinitionKind)
{
    const auto definition = std::string{
        R"({"schemaVersion":1,"componentGuid":"78B71988-2125-4A2B-8B78-02A804570101",)"
        R"("inputs":[],"outputs":[],"config":{},"diagnostics":[]})"};
    const auto entry =
        EntryWithFactoryAndDefinition(kTaskComponentFactoryGuid, definition);

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry(kTaskComponentGuid, entry),
        "taskComponents.components.78B71988-2125-4A2B-8B78-02A804570101."
        "definition.kind: missing required string");
}

TEST(PluginDescTaskComponentsTest, RejectsNonArrayDefinitionInputs)
{
    const auto definition = std::string{
        R"({"schemaVersion":1,"componentGuid":"78B71988-2125-4A2B-8B78-02A804570101",)"
        R"("kind":"branch","inputs":{},"outputs":[],"config":{},"diagnostics":[]})"};
    const auto entry =
        EntryWithFactoryAndDefinition(kTaskComponentFactoryGuid, definition);

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry(kTaskComponentGuid, entry),
        "taskComponents.components.78B71988-2125-4A2B-8B78-02A804570101."
        "definition.inputs: expected array");
}

TEST(PluginDescTaskComponentsTest, RejectsNonArrayDefinitionOutputs)
{
    const auto definition = std::string{
        R"({"schemaVersion":1,"componentGuid":"78B71988-2125-4A2B-8B78-02A804570101",)"
        R"("kind":"branch","inputs":[],"outputs":{},"config":{},"diagnostics":[]})"};
    const auto entry =
        EntryWithFactoryAndDefinition(kTaskComponentFactoryGuid, definition);

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry(kTaskComponentGuid, entry),
        "taskComponents.components.78B71988-2125-4A2B-8B78-02A804570101."
        "definition.outputs: expected array");
}

TEST(PluginDescTaskComponentsTest, RejectsInvalidDefinitionConfigContainer)
{
    const auto definition = std::string{
        R"({"schemaVersion":1,"componentGuid":"78B71988-2125-4A2B-8B78-02A804570101",)"
        R"("kind":"branch","inputs":[],"outputs":[],"config":[],"diagnostics":[]})"};
    const auto entry =
        EntryWithFactoryAndDefinition(kTaskComponentFactoryGuid, definition);

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry(kTaskComponentGuid, entry),
        "taskComponents.components.78B71988-2125-4A2B-8B78-02A804570101."
        "definition.config: expected object");
}

TEST(PluginDescTaskComponentsTest, RejectsInvalidDefinitionDiagnosticsContainer)
{
    const auto definition = std::string{
        R"({"schemaVersion":1,"componentGuid":"78B71988-2125-4A2B-8B78-02A804570101",)"
        R"("kind":"branch","inputs":[],"outputs":[],"config":{},"diagnostics":{}})"};
    const auto entry =
        EntryWithFactoryAndDefinition(kTaskComponentFactoryGuid, definition);

    ExpectInvalidTaskComponents(
        TaskComponentsWithEntry(kTaskComponentGuid, entry),
        "taskComponents.components.78B71988-2125-4A2B-8B78-02A804570101."
        "definition.diagnostics: expected array");
}

TEST(
    PluginDescTaskComponentsTest,
    ParsesCommentsAndTrailingCommasWithManifestFlags)
{
    constexpr auto test_string = R"(
    {
        // Existing manifest load path allows comments.
        "name": "TaskComponentPlugin",
        "author": "Dusk",
        "version": "1.0.0",
        "guid": "0527CD9E-1F26-44FB-BE5F-D63C5A11B754",
        "description": "Plugin with task component manifest",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": [],
        "taskComponents": {
            "factories": [
                "0196057D-1029-4D9D-A2D0-81869F40F721",
            ],
            "components": {
                "78B71988-2125-4A2B-8B78-02A804570101": {
                    "factoryGuid": "0196057D-1029-4D9D-A2D0-81869F40F721",
                    "definition": {
                        "schemaVersion": 1,
                        "componentGuid": "78B71988-2125-4A2B-8B78-02A804570101",
                        "kind": "branch",
                        "inputs": [],
                        "outputs": [],
                        "config": {},
                        "diagnostics": [],
                    },
                },
            }
        },
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string,
            yyjson::ReadFlag::AllowComments
                | yyjson::ReadFlag::AllowTrailingCommas);

    ASSERT_TRUE(desc.task_components.has_value());
    ASSERT_TRUE(desc.task_components->components.has_value());
    EXPECT_TRUE(desc.task_components->components->contains(
        "78B71988-2125-4A2B-8B78-02A804570101"));
}

TEST(PluginDescTaskComponentsTest, DoesNotRequireRejectedPublicFields)
{
    constexpr auto test_string = R"(
    {
        "name": "MinimalTaskComponentPlugin",
        "author": "Dusk",
        "version": "1.0.0",
        "guid": "0527CD9E-1F26-44FB-BE5F-D63C5A11B754",
        "description": "Plugin with minimal official task component fields",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": [],
        "taskComponents": {
            "factories": ["0196057D-1029-4D9D-A2D0-81869F40F721"],
            "components": {
                "78B71988-2125-4A2B-8B78-02A804570101": {
                    "factoryGuid": "0196057D-1029-4D9D-A2D0-81869F40F721",
                    "definition": {
                        "schemaVersion": 1,
                        "componentGuid": "78B71988-2125-4A2B-8B78-02A804570101",
                        "kind": "branch",
                        "inputs": [],
                        "outputs": [],
                        "config": {},
                        "diagnostics": []
                    }
                }
            }
        }
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);

    ASSERT_TRUE(desc.task_components.has_value());
    ASSERT_TRUE(desc.task_components->components.has_value());
    EXPECT_EQ(desc.task_components->components->size(), 1u);
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

// 验证 PluginPackageDescDetailToJson 输出 NativeUI 契约的
// items[]/defaultConfigSchema。 数据源：含 tasks.descriptors 的 manifest（模拟
// IpcTestPlugin1）。 断言每个 task 转为 type:"tool" 的 PluginItem，其
// defaultConfigSchema 含 groups→items
// 结构，且配置项字段（valueType/inputType/defaultValue/options/required）
// 严格匹配 issue DAS-25 约定。
TEST(PluginPackageDescTest, ItemsAndDefaultConfigSchemaFromTaskDescriptors)
{
    constexpr auto test_string = R"(
    {
        "name": "IpcTestPlugin1",
        "author": "Dusk",
        "version": "0.1",
        "guid": "2A3B4C5D-6E7F-4B5C-9D0E-1F2A3B4C5D6E",
        "description": "IPC test plugin 1",
        "supportedSystem": "Windows",
        "language": "Cpp",
        "pluginFilenameExtension": "dll",
        "settings": {},
        "tasks": {
            "A1B2C3D4-E5F6-4A7B-8C9D-0E1F2A3B4C5D": {
                "pluginGuid": "2A3B4C5D-6E7F-4B5C-9D0E-1F2A3B4C5D6E",
                "name": "testTask",
                "description": "A test task for IPC testing",
                "descriptors": [
                    {
                        "name": "retryCount",
                        "type": 0,
                        "defaultValue": 3,
                        "description": "Number of retries",
                        "required": false
                    }
                ]
            }
        }
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);
    ASSERT_EQ(desc.task_descriptors.size(), 1u);

    auto json_val =
        DAS::Core::ForeignInterfaceHost::PluginPackageDescDetailToJson(desc);
    const auto serialized = Das::Utils::SerializeYyjsonValue(json_val, true);
    ASSERT_TRUE(serialized.has_value());
    const auto& out = serialized.value();

    SCOPED_TRACE(out);

    // items[] 顶层字段输出
    EXPECT_NE(out.find("\"items\""), std::string::npos);
    // task → type:"tool" 的 PluginItem
    EXPECT_NE(out.find("\"tool\""), std::string::npos);
    EXPECT_NE(out.find("\"testTask\""), std::string::npos);
    // defaultConfigSchema 与 groups→items 结构
    EXPECT_NE(out.find("\"defaultConfigSchema\""), std::string::npos);
    EXPECT_NE(out.find("\"groups\""), std::string::npos);
    EXPECT_NE(out.find("\"groupId\""), std::string::npos);
    EXPECT_NE(out.find("\"groupName\""), std::string::npos);
    // 配置项字段（NativeUI 契约）
    EXPECT_NE(out.find("\"valueType\""), std::string::npos);
    EXPECT_NE(out.find("\"inputType\""), std::string::npos);
    EXPECT_NE(out.find("\"defaultValue\""), std::string::npos);
    EXPECT_NE(out.find("\"options\""), std::string::npos);
    EXPECT_NE(out.find("\"required\""), std::string::npos);
    // retryCount 配置项出现（type=0 → valueType=0 INT）
    EXPECT_NE(out.find("\"retryCount\""), std::string::npos);
}

// 验证空配置插件（模拟 PythonTestPlugin，settings 为空、无 tasks）
// 不应输出 items 字段（条件输出逻辑）。
TEST(PluginPackageDescTest, NoItemsWhenSettingsAndTasksEmpty)
{
    constexpr auto test_string = R"(
    {
        "name": "PythonTestPlugin",
        "author": "Dusk",
        "version": "0.1",
        "guid": "5E6F7081-9ABC-4E6F-B102-4C5D6E7F8091",
        "description": "Python Test Plugin",
        "supportedSystem": "Windows",
        "language": "Python",
        "pluginFilenameExtension": "py",
        "settings": []
    }
    )";

    const auto desc =
        JsonToStruct<DAS::Core::ForeignInterfaceHost::PluginPackageDesc>(
            test_string);

    auto json_val =
        DAS::Core::ForeignInterfaceHost::PluginPackageDescDetailToJson(desc);
    const auto serialized = Das::Utils::SerializeYyjsonValue(json_val, true);
    ASSERT_TRUE(serialized.has_value());

    // 空配置插件不应输出 items 字段
    EXPECT_EQ(serialized.value().find("\"items\""), std::string::npos);
}

// ─── Phase 84-04: PropertySchema normalization at the public seam ───
//
// 第 8 条已确认 TDD 公共缝 PluginPackageDescDetailToJson（PluginScanner.h，
// DAS_EXPORT）。下面所有断言都只通过该公共符号观察 manifest → 运行时
// PropertySchema 的投影形态，绝不直接测试 PluginScanner.cpp 匿名命名空间内的
// NativeConfigItemFromSetting 私有助手。期望值均为手写 JSON literal，且通过
// 递归比较函数按 object key 无序、array 有序方式比对，独立于生产序列化器的
// 字段输出顺序。

namespace
{
    // 递归结构对比：object key 顺序无关，array 元素顺序敏感。
    // 返回空字符串表示相等；非空字符串描述第一处不匹配。
    // 模板化以同时支持 yyjson::value（mutable_value_base<mutable_document>）
    // 与 yyjson::writer::const_value_ref（const_value_base<const
    // mutable_document&>）：两者通过不同 DocType 模板参数特化，C++ 类型不同，
    // 但都提供 is_object/as_object/operator[]/size 等只读接口；递归调用内部
    // 经 as_object/as_array/operator[] 后取到的子节点统一是 const_*_ref 类型。
    template <typename A, typename B>
    std::string
    DescribeJsonMismatch(const A& a, const B& b, const std::string& path)
    {
        const auto a_is_obj = a.is_object();
        const auto b_is_obj = b.is_object();
        const auto a_is_arr = a.is_array();
        const auto b_is_arr = b.is_array();
        if (a_is_obj != b_is_obj || a_is_arr != b_is_arr)
        {
            return DAS_FMT_NS::format(
                "type mismatch at '{}'",
                path.empty() ? std::string{"<root>"} : path);
        }

        if (a_is_obj)
        {
            const auto a_obj_opt = a.as_object();
            const auto b_obj_opt = b.as_object();
            if (!a_obj_opt || !b_obj_opt)
            {
                return DAS_FMT_NS::format(
                    "object ref failed at '{}'",
                    path.empty() ? std::string{"<root>"} : path);
            }
            const auto& a_obj = *a_obj_opt;
            const auto& b_obj = *b_obj_opt;
            if (a_obj.size() != b_obj.size())
            {
                return DAS_FMT_NS::format(
                    "object size mismatch at '{}': {} vs {}",
                    path.empty() ? std::string{"<root>"} : path,
                    a_obj.size(),
                    b_obj.size());
            }
            for (const auto& [key, value] : a_obj)
            {
                const auto key_sv = std::string_view(key);
                if (!b_obj.contains(key_sv))
                {
                    return DAS_FMT_NS::format(
                        "missing key '{}' at '{}'",
                        std::string{key_sv},
                        path.empty() ? std::string{"<root>"} : path);
                }
                const auto sub = DescribeJsonMismatch(
                    value,
                    b_obj[key_sv],
                    path + "." + std::string{key_sv});
                if (!sub.empty())
                {
                    return sub;
                }
            }
            return {};
        }

        if (a_is_arr)
        {
            const auto a_arr_opt = a.as_array();
            const auto b_arr_opt = b.as_array();
            if (!a_arr_opt || !b_arr_opt)
            {
                return DAS_FMT_NS::format(
                    "array ref failed at '{}'",
                    path.empty() ? std::string{"<root>"} : path);
            }
            const auto& a_arr = *a_arr_opt;
            const auto& b_arr = *b_arr_opt;
            if (a_arr.size() != b_arr.size())
            {
                return DAS_FMT_NS::format(
                    "array size mismatch at '{}': {} vs {}",
                    path.empty() ? std::string{"<root>"} : path,
                    a_arr.size(),
                    b_arr.size());
            }
            for (std::size_t i = 0; i < a_arr.size(); ++i)
            {
                const auto sub = DescribeJsonMismatch(
                    a_arr[i],
                    b_arr[i],
                    DAS_FMT_NS::format("{}[{}]", path, i));
                if (!sub.empty())
                {
                    return sub;
                }
            }
            return {};
        }

        const auto sa = Das::Utils::SerializeYyjsonValue(a, false);
        const auto sb = Das::Utils::SerializeYyjsonValue(b, false);
        if (!sa.has_value() || !sb.has_value() || *sa != *sb)
        {
            return DAS_FMT_NS::format(
                "scalar mismatch at '{}': {} vs {}",
                path.empty() ? std::string{"<root>"} : path,
                sa.value_or("<err>"),
                sb.value_or("<err>"));
        }
        return {};
    }

    // 在 PluginPackageDescDetailToJson 输出里定位首个 PropertySchema 节点：
    //   items[0].defaultConfigSchema.groups[0].items[0]
    // 失败返回 std::nullopt。
    std::optional<yyjson::writer::const_value_ref> FindFirstPropertySchema(
        const yyjson::value& detail_json)
    {
        const auto root_obj_opt = detail_json.as_object();
        if (!root_obj_opt || !root_obj_opt->contains(std::string_view("items")))
        {
            return std::nullopt;
        }
        const auto items_val = (*root_obj_opt)[std::string_view("items")];
        const auto items_arr_opt = items_val.as_array();
        if (!items_arr_opt || items_arr_opt->size() == 0)
        {
            return std::nullopt;
        }
        const auto first_item = (*items_arr_opt)[0];
        const auto first_obj_opt = first_item.as_object();
        if (!first_obj_opt
            || !first_obj_opt->contains(
                std::string_view("defaultConfigSchema")))
        {
            return std::nullopt;
        }
        const auto schema_val =
            (*first_obj_opt)[std::string_view("defaultConfigSchema")];
        const auto schema_obj_opt = schema_val.as_object();
        if (!schema_obj_opt
            || !schema_obj_opt->contains(std::string_view("groups")))
        {
            return std::nullopt;
        }
        const auto groups_val = (*schema_obj_opt)[std::string_view("groups")];
        const auto groups_arr_opt = groups_val.as_array();
        if (!groups_arr_opt || groups_arr_opt->size() == 0)
        {
            return std::nullopt;
        }
        const auto group0 = (*groups_arr_opt)[0];
        const auto group0_obj_opt = group0.as_object();
        if (!group0_obj_opt
            || !group0_obj_opt->contains(std::string_view("items")))
        {
            return std::nullopt;
        }
        const auto group_items_val =
            (*group0_obj_opt)[std::string_view("items")];
        const auto group_items_arr_opt = group_items_val.as_array();
        if (!group_items_arr_opt || group_items_arr_opt->size() == 0)
        {
            return std::nullopt;
        }
        return (*group_items_arr_opt)[0];
    }

    // 构造仅含一个顶层 PluginSettingDesc 的最小 PluginPackageDesc，专供
    // PropertySchema 投影断言使用。settings_groups / task_descriptors 均空，
    // 因此 items[0].defaultConfigSchema.groups[0] 必然是 "general" 分组。
    DAS::Core::ForeignInterfaceHost::PluginPackageDesc
    BuildDescWithSingleTopLevelSetting(
        const DAS::Core::ForeignInterfaceHost::PluginSettingDesc& setting)
    {
        DAS::Core::ForeignInterfaceHost::PluginPackageDesc desc{};
        desc.name = "PropertySchemaTestPlugin";
        desc.author = "test";
        desc.version = "0.1";
        desc.description = "Property schema test fixture";
        desc.supported_system = "Windows";
        desc.language =
            DAS::Core::ForeignInterfaceHost::ForeignInterfaceLanguage::Cpp;
        desc.plugin_filename_extension = "dll";
        desc.guid = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
            "12345678-1234-1234-1234-123456789ABC");
        desc.settings_desc.push_back(setting);
        return desc;
    }

    // 解析手写 JSON literal 到 yyjson::value；失败时抛异常，由调用方在
    // ASSERT_NO_FATAL_FATAL_FAILURE / ADD_FAILURE 中处理。返回值语义干净，
    // 避免在非 void 函数里使用 gtest 的 FAIL() 宏（其内部返回 void）。
    yyjson::value ParseExpectedJson(std::string_view literal)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(literal);
        if (!parsed)
        {
            throw std::runtime_error(
                "Failed to parse expected JSON literal: "
                + std::string{literal});
        }
        return std::move(*parsed);
    }

    // 在 PluginPackageDescDetailToJson 公共缝上断言首个 PropertySchema 节点
    // 与手写 JSON literal 结构相等（object key 顺序无关）。
    void ExpectFirstPropertySchemaEquals(
        const DAS::Core::ForeignInterfaceHost::PluginPackageDesc& desc,
        std::string_view expected_literal)
    {
        auto detail_json =
            DAS::Core::ForeignInterfaceHost::PluginPackageDescDetailToJson(
                desc);
        const auto actual_opt = FindFirstPropertySchema(detail_json);
        ASSERT_TRUE(actual_opt.has_value())
            << "PropertySchema node not found at items[0].defaultConfigSchema."
            << "groups[0].items[0]";

        auto       expected_json = ParseExpectedJson(expected_literal);
        const auto serialized_actual =
            Das::Utils::SerializeYyjsonValue(*actual_opt, true);
        const auto serialized_expected =
            Das::Utils::SerializeYyjsonValue(expected_json, true);
        SCOPED_TRACE(
            "actual:   " + serialized_actual.value_or("<serialize-failed>")
            + "\nexpected: "
            + serialized_expected.value_or("<serialize-failed>"));

        const auto mismatch =
            DescribeJsonMismatch(*actual_opt, expected_json, "");
        EXPECT_TRUE(mismatch.empty())
            << "PropertySchema mismatch: " << mismatch;
    }
} // anonymous namespace

// RED 1: 简单 PluginSettingDesc 投影到 PropertySchema 必须包含
//        name/label/valueType/inputType，且确定性 label 规则是 `label =
//        setting.name`，description 作为 help text 保留；空 enum 时 inputType=0
//        (plain)，无机器回退、无 options 字段。
TEST(PluginPackageDescTest, PropertySchemaDeterministicLabelFromName)
{
    DAS::Core::ForeignInterfaceHost::PluginSettingDesc setting{};
    setting.name = "foo";
    setting.type = Das::ExportInterface::DAS_TYPE_STRING;
    setting.required = false;
    setting.description = "bar";

    const auto desc = BuildDescWithSingleTopLevelSetting(setting);

    ExpectFirstPropertySchemaEquals(
        desc,
        R"({
            "name": "foo",
            "label": "foo",
            "description": "bar",
            "valueType": 4,
            "inputType": 0,
            "defaultValue": null,
            "required": false
        })");
}

// RED 2: enum_values + enum_descriptions 投影为 inputType=1 (select) 加
//        CandidateDescriptor{value,label} 数组，按 enum 声明顺序排列。
//        value 是 setValue 使用的 enum 字符串，label 取自 enum_descriptions
//        （已本地化的显示文本）；缺 enum_descriptions 时 label 回退为 enum
//        value 本身。
TEST(PluginPackageDescTest, PropertySchemaEnumProjectsToSelectCandidates)
{
    DAS::Core::ForeignInterfaceHost::PluginSettingDesc setting{};
    setting.name = "color";
    setting.type = Das::ExportInterface::DAS_TYPE_STRING;
    setting.required = false;
    setting.description = "Pick a color";
    setting.enum_values = std::vector<std::string>{"red", "green", "blue"};
    setting.enum_descriptions =
        std::vector<std::string>{"Red", "Green", "Blue"};

    const auto desc = BuildDescWithSingleTopLevelSetting(setting);

    ExpectFirstPropertySchemaEquals(
        desc,
        R"({
            "name": "color",
            "label": "color",
            "description": "Pick a color",
            "valueType": 4,
            "inputType": 1,
            "defaultValue": null,
            "candidates": [
                {"value": "red",   "label": "Red"},
                {"value": "green", "label": "Green"},
                {"value": "blue",  "label": "Blue"}
            ],
            "required": false
        })");
}

// RED 2b: enum_values 没有 enum_descriptions 时，CandidateDescriptor.label
//         回退为 enum value 本身（manifest 级确定性回退，不是机器 name 暴露
//         给用户——Wave 6 的 MaaPI Provider 会通过 PI case label 提供真正本地
//         化文本）。
TEST(PluginPackageDescTest, PropertySchemaEnumCandidatesFallbackToValueLabel)
{
    DAS::Core::ForeignInterfaceHost::PluginSettingDesc setting{};
    setting.name = "mode";
    setting.type = Das::ExportInterface::DAS_TYPE_STRING;
    setting.required = false;
    setting.description = "Run mode";
    setting.enum_values = std::vector<std::string>{"fast", "slow"};

    const auto desc = BuildDescWithSingleTopLevelSetting(setting);

    ExpectFirstPropertySchemaEquals(
        desc,
        R"({
            "name": "mode",
            "label": "mode",
            "description": "Run mode",
            "valueType": 4,
            "inputType": 1,
            "defaultValue": null,
            "candidates": [
                {"value": "fast", "label": "fast"},
                {"value": "slow", "label": "slow"}
            ],
            "required": false
        })");
}

// RED 3: 手写 manifest 当前不会输出 Phase 84 已删除的运行时旧字段
//        group/dependency/rules/unit/placeholder/deprecationMessage 以及 legacy
//        options 数组（被 CandidateDescriptor.candidates 替换）。即使
//        PluginSettingDesc 携带 deprecation_message，规范化结果也不得把它
//        暴露成运行时字段——manifest 源格式不变，但运行时 PropertySchema 只
//        表达 D-84-18 允许的字段。
TEST(PluginPackageDescTest, PropertySchemaExcludesLegacyRuntimeKeys)
{
    DAS::Core::ForeignInterfaceHost::PluginSettingDesc setting{};
    setting.name = "legacy";
    setting.type = Das::ExportInterface::DAS_TYPE_INT;
    setting.required = true;
    setting.description = "legacy field with deprecation";
    setting.default_value = std::int64_t{42};
    setting.deprecation_message = std::string{"deprecated, do not show"};

    const auto desc = BuildDescWithSingleTopLevelSetting(setting);

    auto detail_json =
        DAS::Core::ForeignInterfaceHost::PluginPackageDescDetailToJson(desc);
    const auto schema_opt = FindFirstPropertySchema(detail_json);
    ASSERT_TRUE(schema_opt.has_value());

    const auto schema_obj_opt = schema_opt->as_object();
    ASSERT_TRUE(schema_obj_opt.has_value());
    const auto& schema_obj = *schema_obj_opt;

    static constexpr std::string_view kLegacyKeys[] = {
        "group",
        "dependency",
        "rules",
        "unit",
        "placeholder",
        "deprecationMessage",
        "options",
    };
    for (const auto& legacy_key : kLegacyKeys)
    {
        EXPECT_FALSE(schema_obj.contains(legacy_key))
            << "legacy runtime key '" << legacy_key
            << "' must be absent from normalized PropertySchema";
    }

    // 已批准字段仍然存在并保持语义。
    EXPECT_TRUE(schema_obj.contains(std::string_view("name")));
    EXPECT_TRUE(schema_obj.contains(std::string_view("label")));
    EXPECT_TRUE(schema_obj.contains(std::string_view("valueType")));
    EXPECT_TRUE(schema_obj.contains(std::string_view("inputType")));
    EXPECT_TRUE(schema_obj.contains(std::string_view("required")));
    EXPECT_TRUE(schema_obj.contains(std::string_view("defaultValue")));
    // label = setting.name 确定性规则
    EXPECT_EQ(
        std::string{schema_obj[std::string_view("label")].as_string().value()},
        "legacy");
}
