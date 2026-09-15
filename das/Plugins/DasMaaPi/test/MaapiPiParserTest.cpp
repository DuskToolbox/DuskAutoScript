#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/PathText.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string_view>

namespace
{
    using namespace Das::Plugins::DasMaaPi;

    std::filesystem::path FixturePath(std::string_view name)
    {
        return std::filesystem::current_path() / DAS_PATH_LITERAL("DasMaaPi")
               / DAS_PATH_LITERAL("test") / DAS_PATH_LITERAL("fixtures")
               / Das::Utils::PathFromUtf8(name);
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
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_v26_jsonc.jsonc")});

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
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_v26_jsonc.jsonc")});

    ASSERT_TRUE(result.ok);
    const auto fixture_dir = FixturePath("").lexically_normal();

    ASSERT_EQ(result.catalog.language_paths.size(), 1u);
    EXPECT_EQ(
        result.catalog.language_paths.at("zh_cn"),
        (fixture_dir / DAS_PATH_LITERAL("languages")
         / DAS_PATH_LITERAL("zh_cn.json"))
            .lexically_normal());
    EXPECT_EQ(
        result.catalog.translations.at("zh_cn").at("task.daily"),
        "日常任务");

    ASSERT_EQ(result.catalog.controllers.size(), 1u);
    ASSERT_EQ(
        result.catalog.controllers[0].resolved_attach_resource_paths.size(),
        1u);
    EXPECT_EQ(
        result.catalog.controllers[0].resolved_attach_resource_paths[0],
        (fixture_dir / DAS_PATH_LITERAL("resource")
         / DAS_PATH_LITERAL("attach"))
            .lexically_normal());

    ASSERT_EQ(result.catalog.resources.size(), 1u);
    ASSERT_EQ(result.catalog.resources[0].resolved_paths.size(), 2u);
    EXPECT_EQ(
        result.catalog.resources[0].resolved_paths[0],
        (fixture_dir / DAS_PATH_LITERAL("resource") / DAS_PATH_LITERAL("base"))
            .lexically_normal());

    EXPECT_NE(
        std::find_if(
            result.catalog.tasks.begin(),
            result.catalog.tasks.end(),
            [](const PiTask& task) { return task.dto.name == "ImportedTask"; }),
        result.catalog.tasks.end());
    EXPECT_NE(
        std::find_if(
            result.catalog.groups.begin(),
            result.catalog.groups.end(),
            [](const PiGroup& group)
            { return group.dto.name == "ImportedGroup"; }),
        result.catalog.groups.end());

    const auto* stage = RequireOption(result.catalog, "stage");
    ASSERT_NE(stage, nullptr);
    ASSERT_EQ(stage->dto.cases.size(), 1u);
    EXPECT_EQ(stage->dto.cases[0].name, "imported-stage");
}

TEST(DasMaaPiCatalog, NormalizeOptionsAndPreserveAgentConfig)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_v26_jsonc.jsonc")});

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
        [](const PiPreset& preset)
        { return preset.dto.name == "DailyPreset"; });
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

// ---------------------------------------------------------------------------
// Phase 86-01: PI v2.7.0 pretask + v2.8.0 setting + import merge (v2.8.0)
// ---------------------------------------------------------------------------

TEST(DasMaaPiParseV28, ParsesPretaskFromSingleObjectAndArray)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_v28.jsonc")});

    ASSERT_TRUE(result.ok) << "v28 fixture should parse without errors";
    // Main contributes a single-object pretask (Bootstrap); the imported
    // fragment contributes an array of two (Warmup, Cleanup). All three must
    // land in catalog.pretasks in source order (main first, then import).
    ASSERT_EQ(result.catalog.pretasks.size(), 3u);
    EXPECT_EQ(result.catalog.pretasks[0].dto.name, "Bootstrap");
    EXPECT_EQ(result.catalog.pretasks[1].dto.name, "Warmup");
    EXPECT_EQ(result.catalog.pretasks[2].dto.name, "Cleanup");
}

TEST(DasMaaPiParseV28, ParsesPretaskFieldsAndFilters)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_v28.jsonc")});

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.catalog.pretasks.size(), 3u);
    const auto& bootstrap = result.catalog.pretasks[0].dto;
    EXPECT_EQ(bootstrap.exec, "tools/bootstrap.exe");
    ASSERT_EQ(bootstrap.args.size(), 2u);
    EXPECT_EQ(bootstrap.args[0], "--init");
    EXPECT_EQ(bootstrap.args[1], "--quiet");
    ASSERT_EQ(bootstrap.option.size(), 1u);
    EXPECT_EQ(bootstrap.option[0], "bootstrap-mode");
    ASSERT_EQ(bootstrap.controller.size(), 1u);
    EXPECT_EQ(bootstrap.controller[0], "Android");
    ASSERT_EQ(bootstrap.resource.size(), 1u);
    EXPECT_EQ(bootstrap.resource[0], "Official");
}

TEST(DasMaaPiParseV28, ParsesSettingsWithDefaultExpandSemantics)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_v28.jsonc")});

    ASSERT_TRUE(result.ok);
    // Main contributes two settings; the imported fragment adds a third.
    ASSERT_EQ(result.catalog.settings.size(), 3u);

    const auto& performance = result.catalog.settings[0].dto;
    EXPECT_EQ(performance.name, "performance");
    ASSERT_EQ(performance.option.size(), 1u);
    EXPECT_EQ(performance.option[0], "stage");
    // default_expand omitted in fixture → protocol default is true.
    EXPECT_TRUE(performance.default_expand);

    const auto& advanced = result.catalog.settings[1].dto;
    EXPECT_EQ(advanced.name, "advanced");
    EXPECT_FALSE(advanced.default_expand);

    const auto& imported = result.catalog.settings[2].dto;
    EXPECT_EQ(imported.name, "imported-setting");
    EXPECT_TRUE(imported.default_expand);
}

TEST(DasMaaPiParseV28, ParsesGroupsWithDefaultExpandSemantics)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_v28.jsonc")});

    ASSERT_TRUE(result.ok);
    // Fixture contributes three groups in source order: Daily (explicit
    // true), Collapsed (default_expand omitted → PI v2 protocol default
    // true), Advanced (explicit false override).
    ASSERT_GE(result.catalog.groups.size(), 2u);

    const auto find_group = [&result](std::string_view name) -> const PiGroup*
    {
        const auto it = std::find_if(
            result.catalog.groups.begin(),
            result.catalog.groups.end(),
            [&name](const PiGroup& group) { return group.dto.name == name; });
        return it == result.catalog.groups.end() ? nullptr : &*it;
    };

    const auto* daily = find_group("Daily");
    ASSERT_NE(daily, nullptr);
    EXPECT_TRUE(daily->dto.default_expand);

    const auto* collapsed = find_group("Collapsed");
    ASSERT_NE(collapsed, nullptr);
    // default_expand omitted → PI v2 protocol default is true.
    EXPECT_TRUE(collapsed->dto.default_expand);

    const auto* advanced = find_group("Advanced");
    ASSERT_NE(advanced, nullptr);
    EXPECT_FALSE(advanced->dto.default_expand);
}

TEST(DasMaaPiParseV28, MergesImportedGlobalOptionSettingPretask)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_v28.jsonc")});

    ASSERT_TRUE(result.ok);
    // global_option merge: main contributes [stage, shared-opt]; import
    // contributes [imported-opt, shared-opt]. shared-opt is deduped (main's
    // wins). Final order: stage, shared-opt, imported-opt.
    ASSERT_EQ(result.catalog.global_options.size(), 3u);
    EXPECT_EQ(result.catalog.global_options[0].dto.name, "stage");
    EXPECT_EQ(result.catalog.global_options[1].dto.name, "shared-opt");
    EXPECT_EQ(result.catalog.global_options[2].dto.name, "imported-opt");

    // pretask ordered append: main's single-object pretask first, then the
    // imported array entries.
    ASSERT_EQ(result.catalog.pretasks.size(), 3u);
    EXPECT_EQ(result.catalog.pretasks[0].dto.name, "Bootstrap");
    EXPECT_EQ(result.catalog.pretasks[1].dto.name, "Warmup");
    EXPECT_EQ(result.catalog.pretasks[2].dto.name, "Cleanup");

    // setting ordered append: main's two, then imported's one.
    ASSERT_EQ(result.catalog.settings.size(), 3u);
    EXPECT_EQ(result.catalog.settings[2].dto.name, "imported-setting");
}

TEST(DasMaaPiParseV28, RejectsPretaskMissingExecAndSettingMissingName)
{
    auto result = ParseProjectInterface(
        {.interface_path = FixturePath("interface_v28_malformed.jsonc")});

    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.catalog.pretasks.size(), 0u);
    ASSERT_EQ(result.catalog.settings.size(), 0u);

    const auto has_code = [&](std::string_view code)
    {
        return std::any_of(
            result.catalog.diagnostics.begin(),
            result.catalog.diagnostics.end(),
            [&](const PiDiagnosticDto& d)
            { return d.severity == "error" && d.code == code; });
    };
    EXPECT_TRUE(has_code("pretask-exec-required"));
    EXPECT_TRUE(has_code("setting-name-required"));
}
