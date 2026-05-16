#include "../src/PluginImpl.h"
#include "FakeMaaApiBoundary.h"

#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <string>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::Test;

    yyjson::value EnvelopeValue(
        bool        fail_fast = true,
        std::string plugin_guid = std::string(kPluginGuidText),
        std::string task_guid = std::string(kTaskGuidText),
        bool        requires_agent = false,
        std::string tasks = R"([
          {"taskName":"DailyFarm","entry":"StartDaily","pipelineOverride":{"Stage":{"value":"one"}}},
          {"taskName":"Base","entry":"StartBase","pipelineOverride":{"Stage":{"value":"two"}}}
        ])")
    {
        auto json =
            R"({"version":1,"pluginGuid":")" + plugin_guid
            + R"(","taskTypeGuid":")" + task_guid
            + R"(","maapi":{"interfaceDirectory":"C:/maa","controllerName":"Android","resourceName":"Official",)"
              R"("resourcePaths":["C:/maa/resource","C:/maa/attach"],"resourceHash":"hash-expected",)"
              R"("failFast":)"
            + std::string(fail_fast ? "true" : "false")
            + R"(,"requiresAgentRuntime":)"
            + std::string(requires_agent ? "true" : "false")
            + R"(,"piEnv":{"controllerJson":"{\"name\":\"Android\",\"type\":\"Adb\"}",)"
              R"("resourceJson":"{\"name\":\"Official\"}"},"tasks":)"
            + tasks + "}}";
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : Das::Utils::MakeYyjsonObject();
    }

    ExecutionEnvelopeDto Envelope(
        bool fail_fast = true,
        bool requires_agent = false,
        std::string tasks = R"([
          {"taskName":"DailyFarm","entry":"StartDaily","pipelineOverride":{"Stage":{"value":"one"}}},
          {"taskName":"Base","entry":"StartBase","pipelineOverride":{"Stage":{"value":"two"}}}
        ])")
    {
        auto parsed = ParseExecutionEnvelope(
            EnvelopeValue(
                fail_fast,
                std::string(kPluginGuidText),
                std::string(kTaskGuidText),
                requires_agent,
                std::move(tasks)));
        EXPECT_EQ(parsed.result, DAS_S_OK);
        return std::move(parsed.envelope);
    }

    DasPtr<ExportInterface::IDasJson> ParseDasJson(std::string json)
    {
        DasPtr<ExportInterface::IDasJson> result;
        EXPECT_EQ(ParseDasJsonFromString(json.c_str(), result.Put()), DAS_S_OK);
        return result;
    }

    std::string EnvelopeJson(
        bool        fail_fast = true,
        std::string plugin_guid = std::string(kPluginGuidText),
        std::string task_guid = std::string(kTaskGuidText),
        bool        requires_agent = false,
        std::string tasks = R"([
          {"taskName":"DailyFarm","entry":"StartDaily","pipelineOverride":{"Stage":{"value":"one"}}}
        ])")
    {
        auto value = EnvelopeValue(
            fail_fast,
            std::move(plugin_guid),
            std::move(task_guid),
            requires_agent,
            std::move(tasks));
        auto serialized = Das::Utils::SerializeYyjsonValue(value);
        return serialized.value_or("{}");
    }

    class RequestedStopToken final
        : public PluginInterface::DasStopTokenImplBase<RequestedStopToken>
    {
    public:
        DasResult StopRequested(bool* canStop) override
        {
            if (!canStop)
            {
                return DAS_E_INVALID_POINTER;
            }
            *canStop = true;
            return DAS_S_OK;
        }
    };
} // namespace

TEST(DasMaaPiRuntime, RuntimeCallSequenceAndCleanup)
{
    FakeMaaApiBoundary fake;
    auto result = MaaRuntime::Run(Envelope(), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_S_OK);
    EXPECT_EQ(
        result.completed_tasks,
        (std::vector<std::string>{"DailyFarm", "Base"}));

    EXPECT_EQ(fake.calls[0], "CreateResource");
    EXPECT_TRUE(fake.Contains("LoadResource:C:/maa/resource"));
    EXPECT_TRUE(fake.Contains("LoadResource:C:/maa/attach"));
    EXPECT_TRUE(fake.Contains("GetResourceHash"));
    EXPECT_TRUE(fake.Contains("CreateController:Android:Adb"));
    EXPECT_TRUE(fake.Contains("CreateTasker"));
    EXPECT_TRUE(fake.Contains("BindResource"));
    EXPECT_TRUE(fake.Contains("BindController"));
    EXPECT_TRUE(fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));
    EXPECT_TRUE(fake.Contains("PostTask:StartBase:{\"Stage\":{\"value\":\"two\"}}"));
    EXPECT_TRUE(fake.Contains("DestroyTasker:3"));
    EXPECT_TRUE(fake.Contains("DestroyController:2"));
    EXPECT_TRUE(fake.Contains("DestroyResource:1"));
}

TEST(DasMaaPiRuntime, RuntimeCleanupOnProviderFailure)
{
    FakeMaaApiBoundary fake;
    fake.post_result_by_entry["StartDaily"] =
        MaaApiResult::Failure(17, "provider post failed");

    auto result = MaaRuntime::Run(Envelope(), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_E_FAIL);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().provider_code.value_or(0), 17);
    EXPECT_TRUE(fake.Contains("DestroyTasker:3"));
    EXPECT_TRUE(fake.Contains("DestroyController:2"));
    EXPECT_TRUE(fake.Contains("DestroyResource:1"));
}

TEST(DasMaaPiRuntime, RuntimeAddsWarningOnResourceHashMismatch)
{
    FakeMaaApiBoundary fake;
    fake.resource_hash = "other-hash";

    auto result = MaaRuntime::Run(Envelope(), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_S_OK);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().severity, "warning");
    EXPECT_EQ(result.diagnostics.front().code, "resource-hash-mismatch");
}

TEST(DasMaaPiRuntime, RuntimeFailFastStopsRemainingTasksByDefault)
{
    FakeMaaApiBoundary fake;
    fake.wait_status_by_entry["StartDaily"] = MaaTaskStatus::Failed;

    auto result = MaaRuntime::Run(Envelope(true), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_E_FAIL);
    EXPECT_TRUE(fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));
    EXPECT_FALSE(fake.Contains("PostTask:StartBase:{\"Stage\":{\"value\":\"two\"}}"));
}

TEST(DasMaaPiRuntime, RuntimeContinuePolicyPostsRemainingTasks)
{
    FakeMaaApiBoundary fake;
    fake.wait_status_by_entry["StartDaily"] = MaaTaskStatus::Failed;

    auto result = MaaRuntime::Run(Envelope(false), fake, nullptr);
    EXPECT_EQ(result.das_result, DAS_E_FAIL);
    EXPECT_TRUE(fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));
    EXPECT_TRUE(fake.Contains("PostTask:StartBase:{\"Stage\":{\"value\":\"two\"}}"));
}

TEST(DasMaaPiRuntime, RuntimeStopTokenMapsToMaaStop)
{
    FakeMaaApiBoundary fake;
    RequestedStopToken stop;

    auto result = MaaRuntime::Run(Envelope(), fake, &stop);
    EXPECT_EQ(result.das_result, DAS_E_FAIL);
    EXPECT_TRUE(result.stopped);
    EXPECT_TRUE(fake.Contains("PostStop"));
    EXPECT_FALSE(fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));
}

TEST(DasMaaPiRuntime, DoRejectsMissingEnvelope)
{
    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, nullptr), DAS_E_INVALID_POINTER);
}

TEST(DasMaaPiRuntime, DoRejectsMalformedJsonEnvelope)
{
    auto json = ParseDasJson("\"not-an-object\"");
    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, json.Get()), DAS_E_INVALID_JSON);
}

TEST(DasMaaPiRuntime, DoRejectsWrongGuid)
{
    auto json = ParseDasJson(
        EnvelopeJson(
            true,
            "00000000-0000-0000-0000-000000000000",
            std::string(kTaskGuidText)));
    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, json.Get()), DAS_E_INVALID_ARGUMENT);
}

TEST(DasMaaPiRuntime, DoRejectsMissingTasks)
{
    auto json = ParseDasJson(EnvelopeJson(true, std::string(kPluginGuidText), std::string(kTaskGuidText), false, "[]"));
    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, json.Get()), DAS_E_INVALID_ARGUMENT);
}

TEST(DasMaaPiRuntime, DoRejectsAgentRequiredEnvelope)
{
    auto json = ParseDasJson(EnvelopeJson(true, std::string(kPluginGuidText), std::string(kTaskGuidText), true));
    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, json.Get()), DAS_E_NO_IMPLEMENTATION);
}

TEST(DasMaaPiRuntime, DoRunsValidNonAgentEnvelope)
{
    FakeMaaApiBoundary fake;
    SetMaaApiBoundaryForTest(&fake);

    auto json = ParseDasJson(EnvelopeJson());
    MaapiTask task;
    EXPECT_EQ(task.Do(nullptr, nullptr, json.Get()), DAS_S_OK);
    EXPECT_TRUE(fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));

    SetMaaApiBoundaryForTest(nullptr);
}
