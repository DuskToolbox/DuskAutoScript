#include "../src/AgentRuntimeService.h"
#include "../src/AgentRuntimeMaaContextResolver.h"
#include "../src/MaaHandle.h"
#include "../src/MaapiAgentComponent.h"
#include "../src/MaapiAgentComponentFactory.h"
#include "../src/MaapiAgentTaskComponent.h"
#include "../src/MaapiAgentTaskComponentFactory.h"
#include "../src/PluginImpl.h"
#include "FakeMaaApiBoundary.h"
#include "IpcTestConfig.h"

#include <Das.ExportInterface.IDasVariantVector.hpp>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::AgentRuntime;
    using namespace Das::Plugins::DasMaaPi::Test;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY;
    using Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;

    struct FakeProcessState
    {
        uint32_t               pid = 5001;
        bool                   running = true;
        std::optional<int32_t> exit_code;
    };

    class FakeProcess final : public IAgentProcess
    {
    public:
        explicit FakeProcess(std::shared_ptr<FakeProcessState> state)
            : state_(std::move(state))
        {
        }

        AgentProcessSnapshot Snapshot() const override
        {
            return AgentProcessSnapshot{
                .running = state_->running,
                .pid = state_->pid,
                .exit_code = state_->exit_code,
                .stdout_tail = "stdout",
                .stderr_tail = "stderr"};
        }

        bool WaitForExit(std::chrono::milliseconds) override
        {
            state_->running = false;
            state_->exit_code = 0;
            return true;
        }

        void Terminate() override { state_->running = false; }

    private:
        std::shared_ptr<FakeProcessState> state_;
    };

    class FakeProcessRunner final : public IAgentProcessRunner
    {
    public:
        AgentProcessLaunchResult Launch(
            const AgentProcessLaunchRequest& request) override
        {
            launches.push_back(request);
            auto state = std::make_shared<FakeProcessState>();
            states.push_back(state);
            return AgentProcessLaunchResult::Success(
                std::make_unique<FakeProcess>(std::move(state)));
        }

        std::vector<AgentProcessLaunchRequest> launches;
        std::vector<std::shared_ptr<FakeProcessState>> states;
    };

    class ScopedRuntimeHooks final
    {
    public:
        ScopedRuntimeHooks(FakeMaaApiBoundary& fake, FakeProcessRunner& runner)
        {
            SetMaaApiBoundaryForTest(&fake);
            SetAgentProcessRunnerForTest(&runner);
        }

        ~ScopedRuntimeHooks()
        {
            SetAgentProcessRunnerForTest(nullptr);
            SetMaaApiBoundaryForTest(nullptr);
        }
    };

    AgentRuntimeMaaContext TestContext()
    {
        return AgentRuntimeMaaContext{
            .resource = 10,
            .controller = 11,
            .tasker = 12};
    }

    DasGuid GuidFrom(std::string_view text)
    {
        DasGuid guid{};
        EXPECT_EQ(
            DasMakeDasGuid(std::string(text).c_str(), &guid),
            DAS_S_OK);
        return guid;
    }

    DasPtr<IDasReadOnlyString> MakeString(std::string_view text)
    {
        DasPtr<IDasReadOnlyString> result;
        EXPECT_EQ(
            CreateIDasReadOnlyStringFromUtf8WithLength(
                text.data(),
                text.size(),
                result.Put()),
            DAS_S_OK);
        return result;
    }

    DasPtr<ExportInterface::IDasVariantVector> MakeStringArgs(
        std::string_view json)
    {
        DasPtr<ExportInterface::IDasVariantVector> args;
        EXPECT_EQ(CreateIDasVariantVector(args.Put()), DAS_S_OK);
        auto text = MakeString(json);
        EXPECT_EQ(args->PushBackString(text.Get()), DAS_S_OK);
        return args;
    }

    std::string ReadFirstString(
        ExportInterface::IDasVariantVector* result)
    {
        EXPECT_NE(result, nullptr);
        EXPECT_EQ(result->GetSize(), 1);

        DasPtr<IDasReadOnlyString> text;
        EXPECT_EQ(result->GetString(0, text.Put()), DAS_S_OK);
        const char* raw = nullptr;
        EXPECT_EQ(text->GetUtf8(&raw), DAS_S_OK);
        return raw ? std::string(raw) : std::string{};
    }

    DasPtr<ExportInterface::IDasJson> ParseJson(std::string_view json)
    {
        DasPtr<ExportInterface::IDasJson> result;
        EXPECT_EQ(
            ParseDasJsonFromString(std::string(json).c_str(), result.Put()),
            DAS_S_OK);
        return result;
    }

    yyjson::value ParseYyjson(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : Das::Utils::MakeYyjsonObject();
    }

    yyjson::value ReadJson(ExportInterface::IDasJson* json)
    {
        DasPtr<IDasReadOnlyString> text;
        EXPECT_EQ(json->ToString(0, text.Put()), DAS_S_OK);
        const char* raw = nullptr;
        EXPECT_EQ(text->GetUtf8(&raw), DAS_S_OK);
        return ParseYyjson(raw ? raw : "{}");
    }

    std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream input(path);
        EXPECT_TRUE(input.is_open()) << path.string();
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

    std::filesystem::path MaapiManifestPath()
    {
        return std::filesystem::path{IpcTestConfig::GetPluginDir()}
               / "DasMaaPi" / "DasMaaPi.json";
    }

    bool ArrayContainsId(
        const yyjson::writer::array_ref& array,
        std::string_view         id)
    {
        return std::any_of(
            array.begin(),
            array.end(),
            [id](const yyjson::value& item)
            {
                auto obj = item.as_object();
                return obj
                       && (*obj)[std::string_view("id")].as_string()
                              .value_or("")
                              == id;
            });
    }

    yyjson::value AgentTaskComponentDefinition()
    {
        auto manifest = ParseYyjson(ReadFile(MaapiManifestPath()));
        auto root = manifest.as_object();
        EXPECT_TRUE(root.has_value());
        auto task_components =
            (*root)[std::string_view("taskComponents")].as_object();
        EXPECT_TRUE(task_components.has_value());
        auto components =
            (*task_components)[std::string_view("components")].as_object();
        EXPECT_TRUE(components.has_value());
        auto entry =
            (*components)[std::string(kAgentTaskComponentGuidText)].as_object();
        EXPECT_TRUE(entry.has_value());
        auto definition =
            (*entry)[std::string_view("definition")].as_object();
        EXPECT_TRUE(definition.has_value());
        const auto serialized =
            (*entry)[std::string_view("definition")].write(
                yyjson::WriteFlag::NoFlag);
        return ParseYyjson(
            std::string_view(serialized.data(), serialized.size()));
    }

    std::string StartRequestJson()
    {
        return R"({
          "version": 1,
          "operation": "start",
          "interfaceDirectory": "C:/maa/project",
          "agent": {
            "childExec": "python",
            "childArgs": ["./agent/main.py"]
          },
          "piEnv": {
            "PI_CLIENT_NAME": "DAS",
            "PI_VERSION": "project"
          }
        })";
    }

    std::string RuntimeRefStartRequestJson()
    {
        return R"({
          "version": 1,
          "operation": "start",
          "runtimeRef": {
            "kind": "maapiRuntimeSession",
            "sessionId": "runtime-1"
          },
          "interfaceDirectory": "C:/maa/project",
          "agent": {
            "childExec": "python",
            "childArgs": ["./agent/main.py"]
          },
          "piEnv": {
            "PI_CLIENT_NAME": "DAS",
            "PI_VERSION": "project"
          }
        })";
    }

    TEST(DasMaaPiAgentPlugin, EnumeratesExistingAndAgentFeatureSurfaces)
    {
        DasMaaPiPlugin plugin;

        const std::vector<Das::PluginInterface::DasPluginFeature> expected{
            DAS_PLUGIN_FEATURE_TASK,
            DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY,
            DAS_PLUGIN_FEATURE_COMPONENT_FACTORY,
            DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY};

        for (std::size_t index = 0; index < expected.size(); ++index)
        {
            Das::PluginInterface::DasPluginFeature feature{};
            ASSERT_EQ(plugin.EnumFeature(index, &feature), DAS_S_OK);
            EXPECT_EQ(feature, expected[index]);
        }

        Das::PluginInterface::DasPluginFeature extra{};
        EXPECT_EQ(plugin.EnumFeature(expected.size(), &extra), DAS_E_OUT_OF_RANGE);
    }

    TEST(DasMaaPiAgentPlugin, CreateFeatureInterfaceReturnsAllFeatureTypes)
    {
        DasMaaPiPlugin plugin;

        DasPtr<IDasBase> task_base;
        ASSERT_EQ(plugin.CreateFeatureInterface(0, task_base.Put()), DAS_S_OK);
        DasPtr<Das::PluginInterface::IDasTask> task;
        EXPECT_EQ(task_base.As(task), DAS_S_OK);

        DasPtr<IDasBase> authoring_base;
        ASSERT_EQ(
            plugin.CreateFeatureInterface(1, authoring_base.Put()),
            DAS_S_OK);
        DasPtr<Das::PluginInterface::IDasTaskAuthoringSessionFactory>
            authoring;
        EXPECT_EQ(authoring_base.As(authoring), DAS_S_OK);

        DasPtr<IDasBase> component_base;
        ASSERT_EQ(
            plugin.CreateFeatureInterface(2, component_base.Put()),
            DAS_S_OK);
        DasPtr<Das::PluginInterface::IDasComponentFactory> component_factory;
        EXPECT_EQ(component_base.As(component_factory), DAS_S_OK);

        DasPtr<IDasBase> task_component_base;
        ASSERT_EQ(
            plugin.CreateFeatureInterface(3, task_component_base.Put()),
            DAS_S_OK);
        DasPtr<Das::PluginInterface::IDasTaskComponentFactory>
            task_component_factory;
        EXPECT_EQ(task_component_base.As(task_component_factory), DAS_S_OK);
    }

    TEST(DasMaaPiAgentComponentFactory, CreatesComponentByStableGuid)
    {
        MaapiAgentComponentFactory factory;
        const auto component_guid = GuidFrom(kAgentComponentGuidText);

        EXPECT_EQ(factory.IsSupported(component_guid), DAS_S_OK);

        DasPtr<Das::PluginInterface::IDasComponent> component;
        ASSERT_EQ(
            factory.CreateInstance(component_guid, component.Put()),
            DAS_S_OK);

        DasGuid actual_guid{};
        ASSERT_EQ(component->GetGuid(&actual_guid), DAS_S_OK);
        EXPECT_EQ(actual_guid, component_guid);
    }

    TEST(
        DasMaaPiAgentComponentFactory,
        CreatedComponentStartsThroughRuntimeRefContext)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        ScopedRuntimeHooks hooks(fake, runner);

        ScopedResource resource(fake, fake.CreateResource());
        ScopedController controller(
            fake,
            fake.CreateController(
                ControllerSpec{.name = "Android", .type = "Adb"}));
        ScopedTasker tasker(fake, fake.CreateTasker());
        ScopedMaaContextRegistration runtime(
            RuntimeRefDto{
                .kind = "maapiRuntimeSession",
                .session_id = "runtime-1"},
            AgentRuntimeMaaContext{
                .resource = resource.get(),
                .controller = controller.get(),
                .tasker = tasker.get()});

        MaapiAgentComponentFactory factory;
        const auto component_guid = GuidFrom(kAgentComponentGuidText);
        DasPtr<Das::PluginInterface::IDasComponent> component;
        ASSERT_EQ(
            factory.CreateInstance(component_guid, component.Put()),
            DAS_S_OK);

        auto function_name = MakeString("start");
        auto args = MakeStringArgs(RuntimeRefStartRequestJson());
        DasPtr<ExportInterface::IDasVariantVector> result;
        ASSERT_EQ(
            component->Dispatch(function_name.Get(), args.Get(), result.Put()),
            DAS_S_OK);

        auto result_json = ParseYyjson(ReadFirstString(result.Get()));
        auto root = result_json.as_object();
        ASSERT_TRUE(root.has_value());
        EXPECT_EQ(
            (*root)[std::string_view("status")].as_string().value_or(""),
            "succeeded");
        ASSERT_EQ(runner.launches.size(), 1u);
        EXPECT_EQ(fake.last_bound_agent_resource, resource.get());
        EXPECT_EQ(fake.last_registered_agent_resource_sink, resource.get());
        EXPECT_EQ(fake.last_registered_agent_controller_sink, controller.get());
        EXPECT_EQ(fake.last_registered_agent_tasker_sink, tasker.get());
    }

    TEST(DasMaaPiAgentComponent, DispatchUsesOneJsonStringInAndOut)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        AgentRuntimeService service(fake, runner);
        MaapiAgentComponent component(service, TestContext());

        auto function_name = MakeString("start");
        auto args = MakeStringArgs(StartRequestJson());
        DasPtr<ExportInterface::IDasVariantVector> result;
        ASSERT_EQ(
            component.Dispatch(function_name.Get(), args.Get(), result.Put()),
            DAS_S_OK);

        auto result_json = ParseYyjson(ReadFirstString(result.Get()));
        auto root = result_json.as_object();
        ASSERT_TRUE(root.has_value());
        EXPECT_EQ(
            (*root)[std::string_view("status")].as_string().value_or(""),
            "succeeded");
        ASSERT_EQ(runner.launches.size(), 1u);
    }

    TEST(DasMaaPiAgentComponent, InvalidJsonReturnsStructuredDiagnostics)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        AgentRuntimeService service(fake, runner);
        MaapiAgentComponent component(service, TestContext());

        auto function_name = MakeString("validate");
        auto args = MakeStringArgs("{not json");
        DasPtr<ExportInterface::IDasVariantVector> result;
        ASSERT_EQ(
            component.Dispatch(function_name.Get(), args.Get(), result.Put()),
            DAS_S_OK);

        auto result_json = ParseYyjson(ReadFirstString(result.Get()));
        auto root = result_json.as_object();
        ASSERT_TRUE(root.has_value());
        EXPECT_EQ(
            (*root)[std::string_view("status")].as_string().value_or(""),
            "failed");
        auto diagnostics =
            (*root)[std::string_view("diagnostics")].as_array();
        ASSERT_TRUE(diagnostics.has_value());
        ASSERT_FALSE(diagnostics->empty());
    }

    TEST(DasMaaPiAgentTaskComponentFactory, CreatesComponentByStableGuid)
    {
        MaapiAgentTaskComponentFactory factory;
        const auto component_guid = GuidFrom(kAgentTaskComponentGuidText);

        DasPtr<Das::PluginInterface::IDasTaskComponent> component;
        ASSERT_EQ(
            factory.CreateComponent(component_guid, component.Put()),
            DAS_S_OK);

        DasGuid actual_guid{};
        ASSERT_EQ(component->GetGuid(&actual_guid), DAS_S_OK);
        EXPECT_EQ(actual_guid, component_guid);
    }

    TEST(
        DasMaaPiAgentTaskComponentFactory,
        CreatedTaskComponentStartsThroughRuntimeRefContext)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        ScopedRuntimeHooks hooks(fake, runner);

        ScopedResource resource(fake, fake.CreateResource());
        ScopedController controller(
            fake,
            fake.CreateController(
                ControllerSpec{.name = "Android", .type = "Adb"}));
        ScopedTasker tasker(fake, fake.CreateTasker());
        ScopedMaaContextRegistration runtime(
            RuntimeRefDto{
                .kind = "maapiRuntimeSession",
                .session_id = "runtime-1"},
            AgentRuntimeMaaContext{
                .resource = resource.get(),
                .controller = controller.get(),
                .tasker = tasker.get()});

        MaapiAgentTaskComponentFactory factory;
        const auto component_guid = GuidFrom(kAgentTaskComponentGuidText);
        DasPtr<Das::PluginInterface::IDasTaskComponent> component;
        ASSERT_EQ(
            factory.CreateComponent(component_guid, component.Put()),
            DAS_S_OK);

        auto input = ParseJson(RuntimeRefStartRequestJson());
        DasPtr<ExportInterface::IDasJson> result;
        ASSERT_EQ(
            component->Do(nullptr, nullptr, nullptr, input.Get(), result.Put()),
            DAS_S_OK);

        auto result_json = ReadJson(result.Get());
        auto root = result_json.as_object();
        ASSERT_TRUE(root.has_value());
        EXPECT_EQ(
            (*root)[std::string_view("status")].as_string().value_or(""),
            "succeeded");
        ASSERT_EQ(runner.launches.size(), 1u);
        EXPECT_EQ(fake.last_bound_agent_resource, resource.get());
        EXPECT_EQ(fake.last_registered_agent_resource_sink, resource.get());
        EXPECT_EQ(fake.last_registered_agent_controller_sink, controller.get());
        EXPECT_EQ(fake.last_registered_agent_tasker_sink, tasker.get());
    }

    TEST(DasMaaPiAgentTaskComponent, ManifestDefinitionUsesStablePortIds)
    {
        auto definition_value = AgentTaskComponentDefinition();
        auto definition = definition_value.as_object();
        ASSERT_TRUE(definition.has_value());

        EXPECT_EQ(
            (*definition)[std::string_view("kind")].as_string().value_or(""),
            "das.maapi.agentRuntime");
        EXPECT_EQ(
            (*definition)[std::string_view("componentGuid")].as_string()
                .value_or(""),
            kAgentTaskComponentGuidText);

        auto settings =
            (*definition)[std::string_view("settings")].as_array();
        ASSERT_TRUE(settings.has_value());
        EXPECT_TRUE(ArrayContainsId(*settings, "tcpCompatMode"));
        EXPECT_TRUE(ArrayContainsId(*settings, "captureOutput"));
        EXPECT_TRUE(ArrayContainsId(*settings, "stopTimeoutMs"));
        EXPECT_TRUE(ArrayContainsId(*settings, "maxOutputTailBytes"));

        auto inputs = (*definition)[std::string_view("inputs")].as_array();
        ASSERT_TRUE(inputs.has_value());
        EXPECT_TRUE(ArrayContainsId(*inputs, "operation"));
        EXPECT_TRUE(ArrayContainsId(*inputs, "runtimeRef"));
        EXPECT_TRUE(ArrayContainsId(*inputs, "interfaceDirectory"));
        EXPECT_TRUE(ArrayContainsId(*inputs, "agents"));
        EXPECT_TRUE(ArrayContainsId(*inputs, "piEnv"));
        EXPECT_TRUE(ArrayContainsId(*inputs, "sessionId"));

        auto outputs = (*definition)[std::string_view("outputs")].as_array();
        ASSERT_TRUE(outputs.has_value());
        EXPECT_TRUE(ArrayContainsId(*outputs, "agentSessionId"));
        EXPECT_TRUE(ArrayContainsId(*outputs, "agents"));
        EXPECT_TRUE(ArrayContainsId(*outputs, "runningAgentCount"));
        EXPECT_TRUE(ArrayContainsId(*outputs, "stdoutTail"));
        EXPECT_TRUE(ArrayContainsId(*outputs, "stderrTail"));

        auto signals = (*definition)[std::string_view("signals")].as_array();
        ASSERT_TRUE(signals.has_value());
        EXPECT_TRUE(ArrayContainsId(*signals, "succeeded"));
        EXPECT_TRUE(ArrayContainsId(*signals, "failed"));
    }

    TEST(DasMaaPiAgentTaskComponent, DoMergesSettingsAndInputThenRunsService)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        AgentRuntimeService service(fake, runner);
        MaapiAgentTaskComponent component(service, TestContext());

        auto settings = ParseJson(R"({
          "captureOutput": false,
          "stopTimeoutMs": 9000,
          "maxOutputTailBytes": 512
        })");
        auto input = ParseJson(StartRequestJson());

        DasPtr<ExportInterface::IDasJson> result;
        ASSERT_EQ(
            component.Do(
                nullptr,
                nullptr,
                settings.Get(),
                input.Get(),
                result.Put()),
            DAS_S_OK);

        auto result_json = ReadJson(result.Get());
        auto root = result_json.as_object();
        ASSERT_TRUE(root.has_value());
        EXPECT_EQ(
            (*root)[std::string_view("status")].as_string().value_or(""),
            "succeeded");
        ASSERT_EQ(runner.launches.size(), 1u);
        EXPECT_FALSE(runner.launches[0].capture_output);
        EXPECT_EQ(runner.launches[0].max_output_tail_bytes, 512u);
    }
} // namespace
