#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/TaskScheduler/FlowControlTaskComponents.h>
#include <das/Core/TaskScheduler/TaskComponentRuntime.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <gtest/gtest.h>

namespace
{
    using Das::DasPtr;

    yyjson::value ToJson(Das::ExportInterface::IDasJson* json)
    {
        DasPtr<IDasReadOnlyString> text;
        EXPECT_EQ(json->ToString(0, text.Put()), DAS_S_OK);
        const char* u8 = nullptr;
        EXPECT_EQ(text->GetUtf8(&u8), DAS_S_OK);
        auto parsed =
            Das::Utils::ParseYyjsonFromString(u8 != nullptr ? u8 : "");
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    DasPtr<Das::ExportInterface::IDasJson> Wrap(yyjson::value value)
    {
        return Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
            std::move(value));
    }

    DasGuid BranchGuid()
    {
        return Das::Core::ForeignInterfaceHost::MakeDasGuid(
            "68F10001-0000-4000-8000-000000000001");
    }
} // namespace

TEST(TaskComponentContractTest, CatalogListsOfficialFlowComponents)
{
    auto factory = Das::Core::TaskScheduler::
        CreateOfficialFlowControlTaskComponentFactory();

    DasPtr<Das::ExportInterface::IDasJson> catalog;
    ASSERT_EQ(factory->GetCatalog(catalog.Put()), DAS_S_OK);

    auto json = ToJson(catalog.Get());
    auto components =
        (*json.as_object())[std::string_view("components")].as_array();
    ASSERT_TRUE(components.has_value());
    ASSERT_EQ(components->size(), 6u);
    EXPECT_EQ(
        (*(*components)[0].as_object())[std::string_view("stableName")]
            .as_string()
            .value(),
        "das.flow.branch");
}

TEST(TaskComponentContractTest, BranchDefinitionIsGraphUsable)
{
    auto factory = Das::Core::TaskScheduler::
        CreateOfficialFlowControlTaskComponentFactory();

    DasPtr<Das::PluginInterface::IDasTaskComponent> component;
    ASSERT_EQ(
        factory->CreateComponent(BranchGuid(), component.Put()),
        DAS_S_OK);

    DasPtr<Das::ExportInterface::IDasJson> definition;
    ASSERT_EQ(component->GetDefinition(definition.Put()), DAS_S_OK);

    auto json = ToJson(definition.Get());
    auto obj = json.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("stableName")].as_string().value(),
        "das.flow.branch");
    EXPECT_EQ((*obj)[std::string_view("kind")].as_string().value(), "branch");
    EXPECT_TRUE((*obj)[std::string_view("traits")].is_object());
    EXPECT_TRUE((*obj)[std::string_view("inputs")].is_array());
    EXPECT_TRUE((*obj)[std::string_view("outputs")].is_array());
    EXPECT_TRUE((*obj)[std::string_view("config")].is_object());
    EXPECT_TRUE((*obj)[std::string_view("diagnostics")].is_array());
}

TEST(TaskComponentContractTest, ApplySettingsChangeReturnsAcceptedSettings)
{
    auto factory = Das::Core::TaskScheduler::
        CreateOfficialFlowControlTaskComponentFactory();

    DasPtr<Das::PluginInterface::IDasTaskComponent> component;
    ASSERT_EQ(
        factory->CreateComponent(BranchGuid(), component.Put()),
        DAS_S_OK);

    auto change = Das::Utils::ParseYyjsonFromString(R"json({
        "kind": "setValue",
        "payload": {"path": "label", "value": "choose route"}
    })json");
    ASSERT_TRUE(change.has_value());
    auto request = Wrap(std::move(*change));

    DasPtr<Das::ExportInterface::IDasJson> result;
    ASSERT_EQ(
        component->ApplySettingsChange(request.Get(), result.Put()),
        DAS_S_OK);

    auto json = ToJson(result.Get());
    EXPECT_EQ(
        (*(*json.as_object())[std::string_view("acceptedSettings")]
              .as_object())[std::string_view("label")]
            .as_string()
            .value(),
        "choose route");
}

TEST(TaskComponentContractTest, BranchDoReturnsStructuredResult)
{
    auto factory = Das::Core::TaskScheduler::
        CreateOfficialFlowControlTaskComponentFactory();

    DasPtr<Das::PluginInterface::IDasTaskComponent> component;
    ASSERT_EQ(
        factory->CreateComponent(BranchGuid(), component.Put()),
        DAS_S_OK);

    auto input =
        Das::Utils::ParseYyjsonFromString(R"json({"condition": true})json");
    ASSERT_TRUE(input.has_value());
    auto input_json = Wrap(std::move(*input));

    DasPtr<Das::ExportInterface::IDasJson> result;
    ASSERT_EQ(
        component
            ->Do(nullptr, nullptr, nullptr, input_json.Get(), result.Put()),
        DAS_S_OK);

    auto json = ToJson(result.Get());
    auto obj = json.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("status")].as_string().value(),
        "completed");
    EXPECT_TRUE((*obj)[std::string_view("outputs")].is_object());
    EXPECT_TRUE((*obj)[std::string_view("diagnostics")].is_array());
    auto signals = (*obj)[std::string_view("signals")].as_array();
    ASSERT_TRUE(signals.has_value());
    ASSERT_EQ(signals->size(), 1u);
    EXPECT_EQ((*signals)[0].as_string().value(), "true");
}

TEST(TaskComponentContractTest, TaskComponentDoesNotExposeDynamicComponent)
{
    auto factory = Das::Core::TaskScheduler::
        CreateOfficialFlowControlTaskComponentFactory();

    DasPtr<Das::PluginInterface::IDasTaskComponent> component;
    ASSERT_EQ(
        factory->CreateComponent(BranchGuid(), component.Put()),
        DAS_S_OK);

    void* out = nullptr;
    EXPECT_EQ(
        component->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasComponent>(),
            &out),
        DAS_E_NO_INTERFACE);
    EXPECT_EQ(out, nullptr);
}
