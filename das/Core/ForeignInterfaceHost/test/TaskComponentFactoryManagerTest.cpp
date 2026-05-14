#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>

#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <gtest/gtest.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

using namespace DAS::Core::ForeignInterfaceHost;
using Das::DasOutPtr;
using Das::DasPtr;
using namespace Das::PluginInterface;

namespace
{
    class CapturingSink final : public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        std::vector<std::string> messages;

    protected:
        void sink_it_(const spdlog::details::log_msg& msg) override
        {
            messages.emplace_back(msg.payload.data(), msg.payload.size());
        }

        void flush_() override {}
    };

    class ScopedLogCapture
    {
    public:
        ScopedLogCapture()
        {
            sink_ = std::make_shared<CapturingSink>();
            DAS::Core::g_logger->sinks().push_back(sink_);
        }

        ~ScopedLogCapture()
        {
            auto& sinks = DAS::Core::g_logger->sinks();
            sinks.erase(
                std::remove(sinks.begin(), sinks.end(), sink_),
                sinks.end());
        }

        bool Contains(std::string_view needle) const
        {
            return std::any_of(
                sink_->messages.begin(),
                sink_->messages.end(),
                [needle](const std::string& message)
                { return message.find(needle) != std::string::npos; });
        }

    private:
        std::shared_ptr<CapturingSink> sink_;
    };

    DasGuid MakeTestGuid(uint32_t value)
    {
        DasGuid guid{};
        guid.data1 = value;
        return guid;
    }

    yyjson::value MakeDefinition(const DasGuid& component_guid)
    {
        auto definition = Das::Utils::MakeYyjsonObject();
        auto obj = *definition.as_object();
        obj[std::string_view("schemaVersion")] = 1;
        obj[std::string_view("componentGuid")] =
            DasGuidToStdString(component_guid);
        obj[std::string_view("kind")] = "testComponent";
        obj[std::string_view("inputs")] = Das::Utils::MakeYyjsonArray();
        obj[std::string_view("outputs")] = Das::Utils::MakeYyjsonArray();
        obj[std::string_view("config")] = Das::Utils::MakeYyjsonObject();
        obj[std::string_view("diagnostics")] = Das::Utils::MakeYyjsonArray();
        return definition;
    }

    TaskComponentsManifestDesc MakeManifest(
        const DasGuid& factory_guid,
        const DasGuid& component_guid)
    {
        TaskComponentsManifestDesc manifest;
        manifest.factories =
            std::vector<std::string>{DasGuidToStdString(factory_guid)};

        TaskComponentManifestEntryDesc entry;
        entry.factory_guid = DasGuidToStdString(factory_guid);
        entry.definition = MakeDefinition(component_guid);

        std::unordered_map<std::string, TaskComponentManifestEntryDesc>
            components;
        components.emplace(DasGuidToStdString(component_guid), std::move(entry));
        manifest.components = std::move(components);
        return manifest;
    }

    class MockTaskComponent final : public IDasTaskComponent
    {
    public:
        explicit MockTaskComponent(DasGuid guid) : guid_(guid) {}

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTypeInfo>())
            {
                *pp_out = static_cast<IDasTypeInfo*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTaskComponent>())
            {
                *pp_out = static_cast<IDasTaskComponent*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (p_out_guid == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = guid_;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (pp_out_name == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            return CreateIDasReadOnlyStringFromUtf8(
                "MockTaskComponent",
                pp_out_name);
        }

        DasResult DAS_STD_CALL ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult DAS_STD_CALL Do(
            IDasStopToken*,
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

    private:
        DasGuid               guid_;
        std::atomic<uint32_t> ref_count_{0};
    };

    class MockTaskComponentFactory final : public IDasTaskComponentFactory
    {
    public:
        explicit MockTaskComponentFactory(DasGuid guid) : guid_(guid) {}

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTypeInfo>())
            {
                *pp_out = static_cast<IDasTypeInfo*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTaskComponentFactory>())
            {
                *pp_out = static_cast<IDasTaskComponentFactory*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (p_out_guid == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = guid_;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (pp_out_name == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            return CreateIDasReadOnlyStringFromUtf8(
                "MockTaskComponentFactory",
                pp_out_name);
        }

        DasResult DAS_STD_CALL
        GetCatalog(Das::ExportInterface::IDasJson**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult DAS_STD_CALL CreateComponent(
            const DasGuid&       component_guid,
            IDasTaskComponent**  pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            ++create_call_count;
            last_component_guid = component_guid;

            if (create_result != DAS_S_OK)
            {
                return create_result;
            }
            auto* component = new MockTaskComponent(component_guid);
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        DasGuid   guid_;
        DasGuid   last_component_guid{};
        DasResult create_result = DAS_S_OK;
        int       create_call_count = 0;

    private:
        std::atomic<uint32_t> ref_count_{0};
    };

    FeatureInfo MakeTaskFactoryFeature(MockTaskComponentFactory& factory)
    {
        FeatureInfo feature{};
        feature.feature_type = DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
        feature.interface_ptr =
            DasPtr<IDasBase>(static_cast<IDasBase*>(&factory));
        return feature;
    }

    class TaskComponentFactoryManagerTest : public ::testing::Test
    {
    protected:
        TaskComponentFactoryManager manager;
    };

    TEST_F(
        TaskComponentFactoryManagerTest,
        RegistersManifestRouteAndCreatesLazilyThroughFactoryGuid)
    {
        const auto plugin_guid = MakeTestGuid(0x68030001);
        const auto factory_guid = MakeTestGuid(0x68030002);
        const auto component_guid = MakeTestGuid(0x68030003);

        auto* factory = new MockTaskComponentFactory(factory_guid);
        factory->AddRef();
        auto feature = MakeTaskFactoryFeature(*factory);
        FeatureInfo* feature_ptr = &feature;
        auto manifest = MakeManifest(factory_guid, component_guid);

        ASSERT_EQ(
            manager.OnPluginLoaded(plugin_guid, {&feature_ptr, 1}, manifest),
            DAS_S_OK);

        EXPECT_EQ(factory->create_call_count, 1)
            << "registration probe should release immediately and not cache "
               "instances";

        DasPtr<IDasTaskComponent> component;
        ASSERT_EQ(
            manager.CreateComponent(component_guid, component.Put()),
            DAS_S_OK);
        EXPECT_NE(component.Get(), nullptr);
        EXPECT_EQ(factory->create_call_count, 2);
        EXPECT_EQ(factory->last_component_guid, component_guid);

        auto definitions = manager.EnumerateDefinitions();
        ASSERT_EQ(definitions.size(), 1u);
        EXPECT_EQ(definitions.front().plugin_guid, plugin_guid);
        EXPECT_EQ(definitions.front().factory_guid, factory_guid);
        EXPECT_EQ(definitions.front().component_guid, component_guid);

        factory->Release();
    }

    TEST_F(TaskComponentFactoryManagerTest, CreateComponentUsesSelectedRoute)
    {
        const auto plugin_guid = MakeTestGuid(0x68030011);
        const auto factory_guid_1 = MakeTestGuid(0x68030012);
        const auto factory_guid_2 = MakeTestGuid(0x68030013);
        const auto component_guid = MakeTestGuid(0x68030014);

        auto* factory_1 = new MockTaskComponentFactory(factory_guid_1);
        auto* factory_2 = new MockTaskComponentFactory(factory_guid_2);
        factory_1->AddRef();
        factory_2->AddRef();

        auto feature_1 = MakeTaskFactoryFeature(*factory_1);
        auto feature_2 = MakeTaskFactoryFeature(*factory_2);
        std::array<FeatureInfo*, 2> features{&feature_1, &feature_2};
        auto manifest = MakeManifest(factory_guid_2, component_guid);
        manifest.factories->push_back(DasGuidToStdString(factory_guid_1));

        ASSERT_EQ(
            manager.OnPluginLoaded(plugin_guid, features, manifest),
            DAS_S_OK);

        DasPtr<IDasTaskComponent> component;
        ASSERT_EQ(
            manager.CreateComponent(component_guid, component.Put()),
            DAS_S_OK);

        EXPECT_EQ(factory_1->create_call_count, 0);
        EXPECT_EQ(factory_2->create_call_count, 2);

        factory_1->Release();
        factory_2->Release();
    }

    TEST_F(TaskComponentFactoryManagerTest, UnloadingPluginClearsRoutes)
    {
        const auto plugin_guid = MakeTestGuid(0x68030021);
        const auto factory_guid = MakeTestGuid(0x68030022);
        const auto component_guid = MakeTestGuid(0x68030023);

        auto* factory = new MockTaskComponentFactory(factory_guid);
        factory->AddRef();
        auto feature = MakeTaskFactoryFeature(*factory);
        FeatureInfo* feature_ptr = &feature;
        auto manifest = MakeManifest(factory_guid, component_guid);

        ASSERT_EQ(
            manager.OnPluginLoaded(plugin_guid, {&feature_ptr, 1}, manifest),
            DAS_S_OK);
        ASSERT_EQ(manager.OnPluginUnloading(plugin_guid), DAS_S_OK);

        DasPtr<IDasTaskComponent> component;
        EXPECT_EQ(
            manager.CreateComponent(component_guid, component.Put()),
            DAS_E_NOT_FOUND);
        EXPECT_TRUE(manager.EnumerateDefinitions().empty());

        factory->Release();
    }

    TEST_F(
        TaskComponentFactoryManagerTest,
        RejectsRouteWhoseFactoryGuidIsNotLoaded)
    {
        const auto plugin_guid = MakeTestGuid(0x68030031);
        const auto loaded_factory_guid = MakeTestGuid(0x68030032);
        const auto declared_factory_guid = MakeTestGuid(0x68030033);
        const auto component_guid = MakeTestGuid(0x68030034);

        auto* factory = new MockTaskComponentFactory(loaded_factory_guid);
        factory->AddRef();
        auto feature = MakeTaskFactoryFeature(*factory);
        FeatureInfo* feature_ptr = &feature;
        auto manifest = MakeManifest(declared_factory_guid, component_guid);

        ScopedLogCapture logs;
        EXPECT_EQ(
            manager.OnPluginLoaded(plugin_guid, {&feature_ptr, 1}, manifest),
            DAS_E_NOT_FOUND);
        EXPECT_TRUE(logs.Contains("factoryGuid"));
        EXPECT_TRUE(logs.Contains("no loaded task component factory"));

        DasPtr<IDasTaskComponent> component;
        EXPECT_EQ(
            manager.CreateComponent(component_guid, component.Put()),
            DAS_E_NOT_FOUND);
        EXPECT_TRUE(manager.EnumerateDefinitions().empty());

        factory->Release();
    }

    TEST_F(
        TaskComponentFactoryManagerTest,
        RejectsCreateComponentProbeFailureWithConcreteReason)
    {
        const auto plugin_guid = MakeTestGuid(0x68030041);
        const auto factory_guid = MakeTestGuid(0x68030042);
        const auto component_guid = MakeTestGuid(0x68030043);

        auto* factory = new MockTaskComponentFactory(factory_guid);
        factory->AddRef();
        factory->create_result = DAS_E_FAIL;
        auto feature = MakeTaskFactoryFeature(*factory);
        FeatureInfo* feature_ptr = &feature;
        auto manifest = MakeManifest(factory_guid, component_guid);

        ScopedLogCapture logs;
        EXPECT_EQ(
            manager.OnPluginLoaded(plugin_guid, {&feature_ptr, 1}, manifest),
            DAS_E_FAIL);
        EXPECT_TRUE(logs.Contains("CreateComponent failed"));
        EXPECT_TRUE(logs.Contains("componentGuid"));

        DasPtr<IDasTaskComponent> component;
        EXPECT_EQ(
            manager.CreateComponent(component_guid, component.Put()),
            DAS_E_NOT_FOUND);

        factory->Release();
    }
} // namespace
