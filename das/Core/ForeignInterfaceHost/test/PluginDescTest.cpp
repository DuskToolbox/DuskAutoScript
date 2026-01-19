#include <cstring>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace
{
    template <class T>
    T JsonToStruct(const std::string& string)
    {
        const auto json = nlohmann::json::parse(string);
        T          result = json.template get<T>();
        return result;
    }
} // Annonymous namespace

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
