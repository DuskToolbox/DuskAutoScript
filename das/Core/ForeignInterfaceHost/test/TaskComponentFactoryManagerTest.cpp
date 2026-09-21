#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>

#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>
#include <gtest/gtest.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
        components.emplace(
            DasGuidToStdString(component_guid),
            std::move(entry));
        manifest.components = std::move(components);
        return manifest;
    }

    void AddManifestComponent(
        TaskComponentsManifestDesc& manifest,
        const DasGuid&              factory_guid,
        const DasGuid&              component_guid)
    {
        TaskComponentManifestEntryDesc entry;
        entry.factory_guid = DasGuidToStdString(factory_guid);
        entry.definition = MakeDefinition(component_guid);
        manifest.components->emplace(
            DasGuidToStdString(component_guid),
            std::move(entry));
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

        DasResult DAS_STD_CALL
        Do(IDasStopToken*,
           Das::ExportInterface::IDasReadOnlyPortMap*,
           Das::ExportInterface::IDasPortMap**) override
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

        DasResult DAS_STD_CALL CreateComponent(
            const DasGuid&      component_guid,
            IDasTaskComponent** pp_out_component) override
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

    class HostAwareTaskComponentFactory final : public IDasTaskComponentFactory
    {
    public:
        HostAwareTaskComponentFactory(
            IDasTaskComponentHost* constructor_host,
            DasGuid                factory_guid,
            DasGuid                parent_component_guid,
            DasGuid                child_component_guid)
            : host(DasPtr<IDasTaskComponentHost>(constructor_host)),
              factory_guid_(factory_guid),
              parent_component_guid_(parent_component_guid),
              child_component_guid_(child_component_guid)
        {
        }

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
                *pp_out = static_cast<IDasBase*>(
                    static_cast<IDasTaskComponentFactory*>(this));
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DasIidOf<IDasTypeInfo>())
            {
                *pp_out = static_cast<IDasTypeInfo*>(
                    static_cast<IDasTaskComponentFactory*>(this));
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
            *p_out_guid = factory_guid_;
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
                "HostAwareTaskComponentFactory",
                pp_out_name);
        }

        DasResult DAS_STD_CALL CreateComponent(
            const DasGuid&      component_guid,
            IDasTaskComponent** pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            ++create_call_count;

            if (enable_reentrant_create
                && component_guid == parent_component_guid_ && host)
            {
                DasPtr<IDasTaskComponent> child;
                host_create_result = host->CreateTaskComponent(
                    child_component_guid_,
                    child.Put());
                child_created_through_host = child.Get() != nullptr;
            }

            auto* component = new MockTaskComponent(component_guid);
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        DasPtr<IDasTaskComponentHost> host;
        DasResult                     host_create_result = DAS_E_FAIL;
        int                           create_call_count = 0;
        bool                          enable_reentrant_create = false;
        bool                          child_created_through_host = false;

    private:
        DasGuid               factory_guid_;
        DasGuid               parent_component_guid_;
        DasGuid               child_component_guid_;
        std::atomic<uint32_t> ref_count_{0};
    };

    FeatureInfo MakeTaskFactoryFeature(IDasTaskComponentFactory& factory)
    {
        FeatureInfo feature{};
        feature.feature_type = DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
        feature.interface_ptr =
            DasPtr<IDasBase>(static_cast<IDasBase*>(&factory));
        return feature;
    }

    // 显式 host 存储的 fake 包：CreateFeatureInterface 以 getter 存量构造
    // 工厂（host 来源 = 包自有存储，加载器保证注入先于枚举）。
    class HostAwareFactoryPackage final
        : public DasPluginPackageImplBase<HostAwareFactoryPackage>
    {
        DasPtr<IDasTaskComponentHost> task_component_host_;

    public:
        HostAwareFactoryPackage(
            DasGuid factory_guid,
            DasGuid parent_component_guid,
            DasGuid child_component_guid)
            : factory_guid_(factory_guid),
              parent_component_guid_(parent_component_guid),
              child_component_guid_(child_component_guid)
        {
        }

        DasResult DAS_STD_CALL
        EnumFeature(uint64_t index, DasPluginFeature* p_out_feature) override
        {
            if (p_out_feature == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (index != 0)
            {
                return DAS_S_FALSE;
            }
            *p_out_feature = DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        CreateFeatureInterface(uint64_t index, IDasBase** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            if (index != 0)
            {
                return DAS_E_NOT_FOUND;
            }

            auto* factory = new HostAwareTaskComponentFactory(
                GetTaskComponentHost(),
                factory_guid_,
                parent_component_guid_,
                child_component_guid_);
            factory->AddRef();
            *pp_out = static_cast<IDasTaskComponentFactory*>(factory);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        SetTaskComponentHost(IDasTaskComponentHost* p_host) override
        {
            task_component_host_ = DasPtr<IDasTaskComponentHost>(p_host);
            return DAS_S_OK;
        }

        IDasTaskComponentHost* GetTaskComponentHost() const
        {
            return task_component_host_.Get();
        }

    private:
        DasGuid factory_guid_;
        DasGuid parent_component_guid_;
        DasGuid child_component_guid_;
    };

    class TaskComponentFactoryManagerTest : public ::testing::Test
    {
    protected:
        TaskComponentFactoryManager manager;

        /// host 本尊 DOM 生命周期用例的辅助：惰性创建真实主进程 IPC 上下文
        /// （仅 host 注册/注销用例调用，既有用例零开销）。创建时刻的本地
        /// 条目数即基线（此前无任何注册）。
        DAS::Core::IPC::MainProcess::IpcContext* GetConcreteIpcContext()
        {
            if (!ipc_context_sp_)
            {
                ipc_context_sp_ =
                    DAS::Core::IPC::MainProcess::CreateIpcContext(false);
                concrete_ipc_context_ =
                    dynamic_cast<DAS::Core::IPC::MainProcess::IpcContext*>(
                        ipc_context_sp_.Get());
                EXPECT_NE(concrete_ipc_context_, nullptr);
                host_dom_baseline_entry_count_ =
                    concrete_ipc_context_->GetProxyFactory()
                        .GetObjectManager()
                        .GetLocalEntryCount();
            }
            return concrete_ipc_context_;
        }

        DAS::Core::IPC::DistributedObjectManager& GetHostDom()
        {
            return GetConcreteIpcContext()
                ->GetProxyFactory()
                .GetObjectManager();
        }

        Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>
        MakeIpcContextRef()
        {
            GetConcreteIpcContext();
            return Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext>(
                ipc_context_sp_.Get());
        }

        DasResult LookupHostObjectId(
            TaskComponentFactoryManager& manager_with_host,
            DAS::Core::IPC::ObjectId&    out_object_id)
        {
            IDasBase* host_as_base = manager_with_host.GetTaskComponentHost();
            return GetHostDom().LookupObjectIdFromPtr(
                host_as_base,
                out_object_id);
        }

        Das::DasPtr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context_sp_;
        DAS::Core::IPC::MainProcess::IpcContext* concrete_ipc_context_ =
            nullptr;
        size_t host_dom_baseline_entry_count_ = 0;
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
        auto         feature = MakeTaskFactoryFeature(*factory);
        FeatureInfo* feature_ptr = &feature;
        auto         manifest = MakeManifest(factory_guid, component_guid);

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
        auto         feature = MakeTaskFactoryFeature(*factory);
        FeatureInfo* feature_ptr = &feature;
        auto         manifest = MakeManifest(factory_guid, component_guid);

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
        auto         feature = MakeTaskFactoryFeature(*factory);
        FeatureInfo* feature_ptr = &feature;
        auto manifest = MakeManifest(declared_factory_guid, component_guid);

        const ScopedLogCapture logs;
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
        RejectsMissingFactoryGuidWithConcreteReason)
    {
        const auto plugin_guid = MakeTestGuid(0x68030035);
        const auto factory_guid = MakeTestGuid(0x68030036);
        const auto component_guid = MakeTestGuid(0x68030037);

        auto* factory = new MockTaskComponentFactory(factory_guid);
        factory->AddRef();
        auto         feature = MakeTaskFactoryFeature(*factory);
        FeatureInfo* feature_ptr = &feature;
        auto         manifest = MakeManifest(factory_guid, component_guid);
        (*manifest.components)
            .at(DasGuidToStdString(component_guid))
            .factory_guid = std::nullopt;

        const ScopedLogCapture logs;
        EXPECT_EQ(
            manager.OnPluginLoaded(plugin_guid, {&feature_ptr, 1}, manifest),
            DAS_E_INVALID_ARGUMENT);
        EXPECT_TRUE(logs.Contains("missing factoryGuid"));
        EXPECT_TRUE(logs.Contains("componentGuid"));

        DasPtr<IDasTaskComponent> component;
        EXPECT_EQ(
            manager.CreateComponent(component_guid, component.Put()),
            DAS_E_NOT_FOUND);

        factory->Release();
    }

    TEST_F(
        TaskComponentFactoryManagerTest,
        RejectsUndeclaredFactoryGuidWithConcreteReason)
    {
        const auto plugin_guid = MakeTestGuid(0x68030038);
        const auto routed_factory_guid = MakeTestGuid(0x68030039);
        const auto declared_factory_guid = MakeTestGuid(0x6803003A);
        const auto component_guid = MakeTestGuid(0x6803003B);

        auto* routed_factory =
            new MockTaskComponentFactory(routed_factory_guid);
        auto* declared_factory =
            new MockTaskComponentFactory(declared_factory_guid);
        routed_factory->AddRef();
        declared_factory->AddRef();
        auto routed_feature = MakeTaskFactoryFeature(*routed_factory);
        auto declared_feature = MakeTaskFactoryFeature(*declared_factory);
        std::array<FeatureInfo*, 2> features{
            &routed_feature,
            &declared_feature};
        auto manifest = MakeManifest(routed_factory_guid, component_guid);
        *manifest.factories =
            std::vector<std::string>{DasGuidToStdString(declared_factory_guid)};

        const ScopedLogCapture logs;
        EXPECT_EQ(
            manager.OnPluginLoaded(plugin_guid, features, manifest),
            DAS_E_INVALID_ARGUMENT);
        EXPECT_TRUE(logs.Contains("undeclared factoryGuid"));
        EXPECT_TRUE(logs.Contains("componentGuid"));

        DasPtr<IDasTaskComponent> component;
        EXPECT_EQ(
            manager.CreateComponent(component_guid, component.Put()),
            DAS_E_NOT_FOUND);

        routed_factory->Release();
        declared_factory->Release();
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
        auto         feature = MakeTaskFactoryFeature(*factory);
        FeatureInfo* feature_ptr = &feature;
        auto         manifest = MakeManifest(factory_guid, component_guid);

        const ScopedLogCapture logs;
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

    TEST_F(
        TaskComponentFactoryManagerTest,
        HostAwareFactoryReceivesHostAndCanCreateChildComponent)
    {
        const auto plugin_guid = MakeTestGuid(0x68030051);
        const auto factory_guid = MakeTestGuid(0x68030052);
        const auto parent_component_guid = MakeTestGuid(0x68030053);
        const auto child_component_guid = MakeTestGuid(0x68030054);

        // 包默认存储路径：包收 host（加载器顺序契约的注入面）→
        // CreateFeatureInterface 构造工厂时传入（构造注入，不经任何对工厂
        // 的事后注入调用）。
        HostAwareFactoryPackage package(
            factory_guid,
            parent_component_guid,
            child_component_guid);
        ASSERT_EQ(
            package.SetTaskComponentHost(manager.GetTaskComponentHost()),
            DAS_S_OK);

        DasPtr<IDasBase> factory_base;
        ASSERT_EQ(
            package.CreateFeatureInterface(0, factory_base.Put()),
            DAS_S_OK);
        DasPtr<IDasTaskComponentFactory> factory;
        ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);
        ASSERT_NE(factory.Get(), nullptr);

        auto* raw_factory =
            static_cast<HostAwareTaskComponentFactory*>(factory.Get());
        auto         feature = MakeTaskFactoryFeature(*factory);
        FeatureInfo* feature_ptr = &feature;
        auto manifest = MakeManifest(factory_guid, parent_component_guid);
        AddManifestComponent(manifest, factory_guid, child_component_guid);

        ASSERT_EQ(
            manager.OnPluginLoaded(plugin_guid, {&feature_ptr, 1}, manifest),
            DAS_S_OK);

        ASSERT_NE(raw_factory->host.Get(), nullptr);
        EXPECT_EQ(raw_factory->host.Get(), manager.GetTaskComponentHost());

        raw_factory->enable_reentrant_create = true;
        DasPtr<IDasTaskComponent> component;
        ASSERT_EQ(
            manager.CreateComponent(parent_component_guid, component.Put()),
            DAS_S_OK);

        EXPECT_NE(component.Get(), nullptr);
        EXPECT_EQ(raw_factory->host_create_result, DAS_S_OK);
        EXPECT_TRUE(raw_factory->child_created_through_host);
    }

    // ============================================================
    // host 本尊的 DOM 显式注册与确定性注销生命周期
    // ============================================================

    TEST_F(
        TaskComponentFactoryManagerTest,
        HostDomInjectedContextRegistersHostExplicitly)
    {
        TaskComponentFactoryManager injected_manager;
        injected_manager.SetIpcContext(MakeIpcContextRef());

        DAS::Core::IPC::ObjectId host_object_id{};
        ASSERT_EQ(
            LookupHostObjectId(injected_manager, host_object_id),
            DAS_S_OK)
            << "host should be explicitly registered after SetIpcContext";

        EXPECT_TRUE(GetHostDom().IsValidObject(host_object_id));
        EXPECT_EQ(
            GetHostDom().GetLocalEntryCount(),
            host_dom_baseline_entry_count_ + 1);

        Das::DasPtr<IDasBase> resolved_host;
        EXPECT_EQ(
            GetHostDom().LookupObject(host_object_id, resolved_host.Put()),
            DAS_S_OK);
        EXPECT_EQ(
            resolved_host.Get(),
            static_cast<IDasBase*>(injected_manager.GetTaskComponentHost()));

        injected_manager.Shutdown();
    }

    TEST_F(
        TaskComponentFactoryManagerTest,
        HostDomShutdownRemovesEntryAndLateLookupsFailCleanly)
    {
        TaskComponentFactoryManager injected_manager;
        injected_manager.SetIpcContext(MakeIpcContextRef());

        DAS::Core::IPC::ObjectId host_object_id{};
        ASSERT_EQ(
            LookupHostObjectId(injected_manager, host_object_id),
            DAS_S_OK);

        injected_manager.Shutdown();

        // 条目移除时 DOM 对该 oid 做 gen bump 作废——本地 session 的迟到
        // Lookup 收 STALE_OBJECT_HANDLE（比 NOT_FOUND 更精确的干净报错，
        // 不会解析进别的对象），"干净报错而非解引用悬垂"锚点成立
        Das::DasPtr<IDasBase> resolved_host;
        EXPECT_EQ(
            GetHostDom().LookupObject(host_object_id, resolved_host.Put()),
            DAS_E_IPC_STALE_OBJECT_HANDLE)
            << "late proxy lookups must fail cleanly after shutdown";
        EXPECT_FALSE(GetHostDom().IsValidObject(host_object_id));
        EXPECT_EQ(
            GetHostDom().GetLocalEntryCount(),
            host_dom_baseline_entry_count_);
    }

    TEST_F(
        TaskComponentFactoryManagerTest,
        HostDomRepeatedShutdownAndDestructionStayIdempotent)
    {
        DAS::Core::IPC::ObjectId host_object_id{};
        {
            TaskComponentFactoryManager injected_manager;
            injected_manager.SetIpcContext(MakeIpcContextRef());
            ASSERT_EQ(
                LookupHostObjectId(injected_manager, host_object_id),
                DAS_S_OK);
            EXPECT_EQ(
                GetHostDom().GetLocalEntryCount(),
                host_dom_baseline_entry_count_ + 1);

            injected_manager.Shutdown();
            injected_manager.Shutdown();
            EXPECT_EQ(
                GetHostDom().GetLocalEntryCount(),
                host_dom_baseline_entry_count_);
        }
        // 析构兜底注销是第三次注销尝试：不得产生副作用或崩溃
        EXPECT_EQ(
            GetHostDom().GetLocalEntryCount(),
            host_dom_baseline_entry_count_);
        // gen bump 作废后的迟到 Lookup 收 STALE_OBJECT_HANDLE（干净报错）
        Das::DasPtr<IDasBase> resolved_host;
        EXPECT_EQ(
            GetHostDom().LookupObject(host_object_id, resolved_host.Put()),
            DAS_E_IPC_STALE_OBJECT_HANDLE);
    }

    TEST_F(
        TaskComponentFactoryManagerTest,
        HostDomMissingContextInjectionSkipsSafely)
    {
        // 夹具自带的 manager 未注入 context：注册跳过、注销跳过、析构兜底
        // 跳过——防御分支不崩溃
        EXPECT_NE(manager.GetTaskComponentHost(), nullptr);

        DAS::Core::IPC::ObjectId host_object_id{};
        EXPECT_EQ(LookupHostObjectId(manager, host_object_id), DAS_E_NOT_FOUND)
            << "without injected context the host must not enter the DOM";

        manager.Shutdown();
        EXPECT_EQ(
            GetHostDom().GetLocalEntryCount(),
            host_dom_baseline_entry_count_);
    }

    // ============================================================
    // creator 后端解析注入（CreateAuthoringSession typed 双法 + D-03
    // 缺注入具名错）
    // ============================================================

    class MockAuthoringSession final : public IDasTaskAuthoringSession
    {
    public:
        explicit MockAuthoringSession(DasGuid guid) : guid_(guid) {}

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
            if (iid == DasIidOf<IDasTaskAuthoringSession>())
            {
                *pp_out = static_cast<IDasTaskAuthoringSession*>(this);
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
                "MockAuthoringSession",
                pp_out_name);
        }

        DasResult DAS_STD_CALL GetDocument(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_document) override
        {
            if (pp_out_document == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_document = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult DAS_STD_CALL ApplyChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result) override
        {
            if (pp_out_result == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_result = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult DAS_STD_CALL Compile(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result) override
        {
            if (pp_out_result == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_result = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }

    private:
        DasGuid               guid_;
        std::atomic<uint32_t> ref_count_{0};
    };

    TEST_F(
        TaskComponentFactoryManagerTest,
        CreateAuthoringSessionWithoutBackendReturnsNamedError)
    {
        // D-03：会话工厂解析后端未注入 → 具名显性错（经 host 本尊走全链）
        auto* host = manager.GetTaskComponentHost();
        ASSERT_NE(host, nullptr);

        const auto                       task_guid = MakeTestGuid(0x68030061);
        DasPtr<IDasTaskAuthoringSession> session;
        const ScopedLogCapture           logs;
        EXPECT_EQ(
            host->CreateAuthoringSession(task_guid, nullptr, session.Put()),
            DAS_E_AUTHORING_SESSION_RESOLVER_MISSING);
        EXPECT_FALSE(session);
        EXPECT_TRUE(logs.Contains("CreateAuthoringSession backend resolver"));
    }

    TEST_F(
        TaskComponentFactoryManagerTest,
        ResolveRepositoryEntryWithoutBackendReturnsNamedError)
    {
        // D-03：entryId→条目解析后端未注入 → 具名显性错
        RepositoryEntrySummary summary;
        const ScopedLogCapture logs;
        EXPECT_EQ(
            manager.ResolveRepositoryEntry(7, &summary),
            DAS_E_REPOSITORY_ENTRY_RESOLVER_MISSING);
    }

    TEST_F(
        TaskComponentFactoryManagerTest,
        CreateAuthoringSessionWithBackendReturnsSession)
    {
        // 成功路径：fake 后端注入 → host 本尊 CreateAuthoringSession 返回
        // 后端产物会话
        const auto task_guid = MakeTestGuid(0x68030071);

        auto* session_impl = new MockAuthoringSession(task_guid);
        session_impl->AddRef();
        int session_resolver_calls = 0;
        manager.SetBackendResolvers(
            RepositoryEntryResolver{},
            [&session_resolver_calls, session_impl](
                const DasGuid&                  resolved_task_guid,
                Das::ExportInterface::IDasJson* p_context_json,
                IDasTaskAuthoringSession**      pp_out_session)
            {
                ++session_resolver_calls;
                EXPECT_EQ(resolved_task_guid, MakeTestGuid(0x68030071));
                EXPECT_EQ(p_context_json, nullptr);
                *pp_out_session = session_impl;
                (*pp_out_session)->AddRef();
                return DAS_S_OK;
            });

        auto*                            host = manager.GetTaskComponentHost();
        DasPtr<IDasTaskAuthoringSession> session;
        ASSERT_EQ(
            host->CreateAuthoringSession(task_guid, nullptr, session.Put()),
            DAS_S_OK);
        EXPECT_EQ(session_resolver_calls, 1);
        EXPECT_EQ(session.Get(), session_impl);

        session_impl->Release();
        manager.SetBackendResolvers({}, {});
    }

    TEST_F(
        TaskComponentFactoryManagerTest,
        ResolveRepositoryEntryWithBackendReturnsSummary)
    {
        // 成功路径：fake 后端注入 → 条目最小字段集原样到达
        RepositoryEntrySummary fake_summary;
        fake_summary.entry_id = 42;
        fake_summary.task_type_guid = "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
        fake_summary.accepted_properties = Das::Utils::MakeYyjsonObject();
        fake_summary.availability_state = "available";
        fake_summary.availability_reason = "";

        int entry_resolver_calls = 0;
        manager.SetBackendResolvers(
            [&entry_resolver_calls, &fake_summary](int64_t entry_id)
            {
                ++entry_resolver_calls;
                EXPECT_EQ(entry_id, 42);
                return std::optional<RepositoryEntrySummary>(fake_summary);
            },
            AuthoringSessionResolver{});

        RepositoryEntrySummary resolved;
        ASSERT_EQ(manager.ResolveRepositoryEntry(42, &resolved), DAS_S_OK);
        EXPECT_EQ(entry_resolver_calls, 1);
        EXPECT_EQ(resolved.entry_id, 42);
        EXPECT_EQ(
            resolved.task_type_guid,
            "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3");
        EXPECT_EQ(resolved.availability_state, "available");

        // 后端报条目不存在 → NOT_FOUND（与缺注入的具名码区分）
        manager.SetBackendResolvers(
            [](int64_t) { return std::optional<RepositoryEntrySummary>{}; },
            AuthoringSessionResolver{});
        EXPECT_EQ(
            manager.ResolveRepositoryEntry(43, &resolved),
            DAS_E_NOT_FOUND);
    }

    TEST_F(
        TaskComponentFactoryManagerTest,
        BackendErrorLensCarriesEnAndZhCnForNamedCodes)
    {
        // D-03 ErrorLens 人话条目：两个具名码 en/zh-cn 双语非空
        auto lens = CreateRegisteredTaskComponentBackendErrorLens();
        ASSERT_TRUE(lens);

        const auto message_of = [&lens](const char* locale, DasResult code)
        {
            const DasReadOnlyString    locale_str{locale};
            DasPtr<IDasReadOnlyString> out;
            if (DAS::IsFailed(
                    lens->GetErrorMessage(locale_str.Get(), code, out.Put())))
            {
                return std::string{};
            }
            const char* text = nullptr;
            EXPECT_EQ(out->GetUtf8(&text), DAS_S_OK);
            return text != nullptr ? std::string{text} : std::string{};
        };

        EXPECT_FALSE(
            message_of("en", DAS_E_REPOSITORY_ENTRY_RESOLVER_MISSING).empty());
        EXPECT_FALSE(
            message_of("zh-cn", DAS_E_REPOSITORY_ENTRY_RESOLVER_MISSING)
                .empty());
        EXPECT_FALSE(
            message_of("en", DAS_E_AUTHORING_SESSION_RESOLVER_MISSING).empty());
        EXPECT_FALSE(
            message_of("zh-cn", DAS_E_AUTHORING_SESSION_RESOLVER_MISSING)
                .empty());
    }
} // namespace
