#include <das/Plugins/DasMaaPi/PiParser.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string_view>

namespace
{
    using namespace Das::Plugins::DasMaaPi;

    std::filesystem::path FixturePath(std::string_view name)
    {
        return std::filesystem::current_path() / "DasMaaPi" / "test"
               / "fixtures" / std::filesystem::path{name};
    }

    const PiOption* RequireOption(
        const PiCatalog& catalog,
        std::string_view name)
    {
        const auto* option = FindOption(catalog, name);
        EXPECT_NE(option, nullptr);
        return option;
    }
} // namespace

TEST(DasMaaPiParse, ParseJsoncV26PreservesUnknowns)
{
    auto result =
        ParseProjectInterface({.interface_path = FixturePath("interface_v26_jsonc.jsonc")});

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.catalog.name, "DasFixture");
    EXPECT_EQ(result.catalog.version, "2.6.0");
    ASSERT_EQ(result.catalog.resources.size(), 1u);
    EXPECT_EQ(result.catalog.resources[0].dto.hash, "sha256:abc123");
    EXPECT_FALSE(result.catalog.raw_agent_json.empty());
    EXPECT_NE(
        result.catalog.raw_agent_json.find("child_exec"),
        std::string::npos);

    EXPECT_NE(
        std::find(
            result.catalog.raw.unknown_fields.begin(),
            result.catalog.raw.unknown_fields.end(),
            "future_top_level"),
        result.catalog.raw.unknown_fields.end());
    EXPECT_NE(
        std::find(
            result.catalog.resources[0].raw.unknown_fields.begin(),
            result.catalog.resources[0].raw.unknown_fields.end(),
            "future_resource"),
        result.catalog.resources[0].raw.unknown_fields.end());
}

TEST(DasMaaPiCatalog, ResolveRelativePathsAndImportCatalog)
{
    auto result =
        ParseProjectInterface({.interface_path = FixturePath("interface_v26_jsonc.jsonc")});

    ASSERT_TRUE(result.ok);
    const auto fixture_dir = FixturePath("").lexically_normal();

    ASSERT_EQ(result.catalog.language_paths.size(), 1u);
    EXPECT_EQ(
        result.catalog.language_paths.at("zh_cn"),
        (fixture_dir / "languages" / "zh_cn.json").lexically_normal());
    EXPECT_EQ(
        result.catalog.translations.at("zh_cn").at("task.daily"),
        "日常任务");

    ASSERT_EQ(result.catalog.controllers.size(), 1u);
    ASSERT_EQ(
        result.catalog.controllers[0].resolved_attach_resource_paths.size(),
        1u);
    EXPECT_EQ(
        result.catalog.controllers[0].resolved_attach_resource_paths[0],
        (fixture_dir / "resource" / "attach").lexically_normal());

    ASSERT_EQ(result.catalog.resources.size(), 1u);
    ASSERT_EQ(result.catalog.resources[0].resolved_paths.size(), 2u);
    EXPECT_EQ(
        result.catalog.resources[0].resolved_paths[0],
        (fixture_dir / "resource" / "base").lexically_normal());

    EXPECT_NE(
        std::find_if(
            result.catalog.tasks.begin(),
            result.catalog.tasks.end(),
            [](const PiTask& task) {
                return task.dto.name == "ImportedTask";
            }),
        result.catalog.tasks.end());
    EXPECT_NE(
        std::find_if(
            result.catalog.groups.begin(),
            result.catalog.groups.end(),
            [](const PiGroup& group) {
                return group.dto.name == "ImportedGroup";
            }),
        result.catalog.groups.end());

    const auto* stage = RequireOption(result.catalog, "stage");
    ASSERT_NE(stage, nullptr);
    ASSERT_EQ(stage->dto.cases.size(), 1u);
    EXPECT_EQ(stage->dto.cases[0].name, "imported-stage");
}

TEST(DasMaaPiCatalog, NormalizeOptionsAndPreserveAgentConfig)
{
    auto result =
        ParseProjectInterface({.interface_path = FixturePath("interface_v26_jsonc.jsonc")});

    ASSERT_TRUE(result.ok);
    ASSERT_FALSE(result.catalog.raw_agent_json.empty());

    const auto* retry = RequireOption(result.catalog, "retry");
    ASSERT_NE(retry, nullptr);
    ASSERT_EQ(retry->dto.inputs.size(), 1u);
    EXPECT_EQ(retry->dto.inputs[0].name, "times");
    EXPECT_EQ(retry->dto.inputs[0].pipeline_type, "int");
    EXPECT_FALSE(retry->raw_pipeline_override_json.empty());

    const auto* checkbox = RequireOption(result.catalog, "use-medicine");
    ASSERT_NE(checkbox, nullptr);
    ASSERT_EQ(checkbox->dto.cases.size(), 2u);
    EXPECT_EQ(checkbox->dto.cases[0].name, "small");
    EXPECT_EQ(checkbox->dto.cases[1].name, "large");
    ASSERT_EQ(checkbox->dto.default_cases.size(), 1u);
    EXPECT_EQ(checkbox->dto.default_cases[0], "large");

    const auto* notify = RequireOption(result.catalog, "notify");
    ASSERT_NE(notify, nullptr);
    EXPECT_EQ(notify->dto.type, "switch");
    ASSERT_EQ(notify->dto.controller.size(), 1u);
    EXPECT_EQ(notify->dto.controller[0], "Android");

    ASSERT_EQ(result.catalog.presets.size(), 2u);
    const auto daily = std::find_if(
        result.catalog.presets.begin(),
        result.catalog.presets.end(),
        [](const PiPreset& preset) {
            return preset.dto.name == "DailyPreset";
        });
    ASSERT_NE(daily, result.catalog.presets.end());
    ASSERT_EQ(daily->dto.task.size(), 1u);
    EXPECT_NE(
        std::find(
            daily->dto.task[0].option_names.begin(),
            daily->dto.task[0].option_names.end(),
            "stage"),
        daily->dto.task[0].option_names.end());

    ASSERT_EQ(result.catalog.global_options.size(), 1u);
    EXPECT_EQ(result.catalog.global_options[0].dto.name, "stage");
}
