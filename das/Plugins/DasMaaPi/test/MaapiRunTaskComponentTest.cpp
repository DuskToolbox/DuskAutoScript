#include "../src/MaapiRunTaskComponent.h"
#include "../src/PluginUtils.h"
#include "FakeMaaApiBoundary.h"

#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::Test;

    DasPtr<ExportInterface::IDasPortMap> MakeInputPortMap(
        std::string_view execution_input_json)
    {
        DasPtr<ExportInterface::IDasPortMap> input_map;
        EXPECT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);

        DasReadOnlyString key{"executionInput"};
        DasReadOnlyString val{std::string{execution_input_json}.c_str()};
        input_map->SetString(key.Get(), val.Get());
        return input_map;
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

// After Do() ABI change to PortMap, MaapiRunTaskComponent::Do() is stubbed
// with DAS_E_NO_IMPLEMENTATION. These tests verify the stub behavior until
// the full PortMap-based implementation is ready.

TEST(DasMaaPiRunTaskComponent, DoReturnsNoImplementation)
{
    MaapiRunTaskComponent component;
    auto                  input = MakeInputPortMap(EnvelopeJson());

    DasPtr<ExportInterface::IDasPortMap> result;
    EXPECT_EQ(
        component.Do(nullptr, input.Get(), result.Put()),
        DAS_E_NO_IMPLEMENTATION);
}

TEST(DasMaaPiRunTaskComponent, DoStubsWithoutCrashOnNullInput)
{
    MaapiRunTaskComponent component;

    DasPtr<ExportInterface::IDasPortMap> result;
    EXPECT_EQ(
        component.Do(nullptr, nullptr, result.Put()),
        DAS_E_NO_IMPLEMENTATION);
}
