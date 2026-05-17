#include "../src/MaapiRunTaskComponent.h"
#include "../src/PluginUtils.h"
#include "FakeMaaApiBoundary.h"

#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::Test;

    yyjson::value ParseYyjson(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : Das::Utils::MakeYyjsonObject();
    }

    DasPtr<ExportInterface::IDasJson> ParseJson(std::string_view json)
    {
        return WrapJson(ParseYyjson(json));
    }

    yyjson::value ReadResult(ExportInterface::IDasJson* json)
    {
        auto parsed = ReadJson(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : Das::Utils::MakeYyjsonObject();
    }

    std::string EnvelopeJson()
    {
        return R"json({
          "version": 1,
          "pluginGuid": "69F20000-0000-4000-8000-000000000001",
          "taskTypeGuid": "69F20001-0000-4000-8000-000000000001",
          "maapi": {
            "interfaceDirectory": "C:/maa",
            "controllerName": "Android",
            "resourceName": "Official",
            "resourcePaths": ["C:/maa/resource"],
            "resourceHash": "hash-expected",
            "failFast": true,
            "requiresAgentRuntime": false,
            "piEnv": {
              "controllerJson": "{\"name\":\"Android\",\"type\":\"Adb\"}",
              "resourceJson": "{\"name\":\"Official\"}"
            },
            "tasks": [
              {
                "taskName": "DailyFarm",
                "entry": "StartDaily",
                "pipelineOverride": {"Stage": {"value": "one"}}
              },
              {
                "taskName": "Base",
                "entry": "StartBase",
                "pipelineOverride": {}
              }
            ]
          }
        })json";
    }

    DasPtr<ExportInterface::IDasJson> ExecutionInput()
    {
        return ParseJson(
            std::string(R"json({"executionInput":)json") + EnvelopeJson()
            + "}");
    }

    std::string DiagnosticCode(const yyjson::value& result)
    {
        auto obj = result.as_object();
        if (!obj)
        {
            return {};
        }
        auto diagnostics = (*obj)[std::string_view("diagnostics")].as_array();
        if (!diagnostics || diagnostics->empty())
        {
            return {};
        }
        auto first = (*diagnostics)[0].as_object();
        if (!first)
        {
            return {};
        }
        return std::string(
            (*first)[std::string_view("code")].as_string().value_or(""));
    }

    class RequestedStopToken final
        : public PluginInterface::DasStopTokenImplBase<RequestedStopToken>
    {
    public:
        DasResult StopRequested(bool* p_out_stop_requested) override
        {
            if (p_out_stop_requested == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_stop_requested = true;
            return DAS_S_OK;
        }
    };

    class ScopedBoundaryHook final
    {
    public:
        explicit ScopedBoundaryHook(FakeMaaApiBoundary& boundary)
        {
            SetMaaApiBoundaryForTest(&boundary);
        }

        ~ScopedBoundaryHook() { SetMaaApiBoundaryForTest(nullptr); }
    };
} // namespace

TEST(DasMaaPiRunTaskComponent, DoRunsCompiledEnvelopeThroughMaaRuntime)
{
    FakeMaaApiBoundary    fake;
    ScopedBoundaryHook    hook(fake);
    MaapiRunTaskComponent component;

    auto                              input = ExecutionInput();
    DasPtr<ExportInterface::IDasJson> result;
    ASSERT_EQ(
        component.Do(nullptr, nullptr, nullptr, input.Get(), result.Put()),
        DAS_S_OK);

    auto json = ReadResult(result.Get());
    auto obj = json.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("status")].as_string().value_or(""),
        "completed");
    auto outputs = (*obj)[std::string_view("outputs")].as_object();
    ASSERT_TRUE(outputs.has_value());
    auto completed = (*outputs)[std::string_view("completedTasks")].as_array();
    ASSERT_TRUE(completed.has_value());
    ASSERT_EQ(completed->size(), 2u);
    EXPECT_EQ((*completed)[0].as_string().value_or(""), "DailyFarm");
    EXPECT_EQ((*completed)[1].as_string().value_or(""), "Base");
    auto diagnostics = (*obj)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diagnostics.has_value());
    EXPECT_TRUE(diagnostics->empty());
    auto signals = (*obj)[std::string_view("signals")].as_object();
    ASSERT_TRUE(signals.has_value());
    EXPECT_TRUE(
        (*signals)[std::string_view("succeeded")].as_bool().value_or(false));
    EXPECT_TRUE(
        fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));
    EXPECT_TRUE(fake.Contains("PostTask:StartBase:{}"));
}

TEST(DasMaaPiRunTaskComponent, DoRejectsInvalidEnvelopeWithDiagnostic)
{
    MaapiRunTaskComponent component;
    auto input = ParseJson(R"json({"executionInput": {}})json");

    DasPtr<ExportInterface::IDasJson> result;
    ASSERT_EQ(
        component.Do(nullptr, nullptr, nullptr, input.Get(), result.Put()),
        DAS_S_OK);

    auto json = ReadResult(result.Get());
    auto obj = json.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("status")].as_string().value_or(""),
        "failed");
    EXPECT_EQ(DiagnosticCode(json), "invalid-envelope");
    auto signals = (*obj)[std::string_view("signals")].as_object();
    ASSERT_TRUE(signals.has_value());
    EXPECT_TRUE(
        (*signals)[std::string_view("failed")].as_bool().value_or(false));
}

TEST(DasMaaPiRunTaskComponent, DoMapsStoppedRuntimeToCancelledSignal)
{
    FakeMaaApiBoundary    fake;
    ScopedBoundaryHook    hook(fake);
    MaapiRunTaskComponent component;
    RequestedStopToken    stop_token;

    auto                              input = ExecutionInput();
    DasPtr<ExportInterface::IDasJson> result;
    ASSERT_EQ(
        component.Do(&stop_token, nullptr, nullptr, input.Get(), result.Put()),
        DAS_S_OK);

    auto json = ReadResult(result.Get());
    auto obj = json.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("status")].as_string().value_or(""),
        "cancelled");
    auto outputs = (*obj)[std::string_view("outputs")].as_object();
    ASSERT_TRUE(outputs.has_value());
    EXPECT_TRUE(
        (*outputs)[std::string_view("stopped")].as_bool().value_or(false));
    auto signals = (*obj)[std::string_view("signals")].as_object();
    ASSERT_TRUE(signals.has_value());
    EXPECT_TRUE(
        (*signals)[std::string_view("cancelled")].as_bool().value_or(false));
    EXPECT_TRUE(fake.Contains("PostStop"));
    EXPECT_FALSE(
        fake.Contains("PostTask:StartDaily:{\"Stage\":{\"value\":\"one\"}}"));
}
