#include <das/Core/ForeignInterfaceHost/ComponentFactoryManager.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace DAS::Core::ForeignInterfaceHost;
using Das::DasPtr;
using namespace Das::PluginInterface;

namespace
{
    // Minimal mock IDasComponent — only refcount + QI + Dispatch
    class MockComponent final : public IDasComponent
    {
        std::atomic<uint32_t> ref_{0};

    public:
        uint32_t DAS_STD_CALL AddRef() override { return ++ref_; }
        uint32_t DAS_STD_CALL Release() override
        {
            auto c = --ref_;
            if (c == 0)
            {
                delete this;
            }
            return c;
        }
        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp) override
        {
            if (!pp)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp = this;
            AddRef();
            return DAS_S_OK;
        }
        DasResult DAS_STD_CALL GetGuid(DasGuid* p) override
        {
            if (p)
            {
                *p = DasGuid{};
            }
            return DAS_S_OK;
        }
        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
        DasResult DAS_STD_CALL Dispatch(
            IDasReadOnlyString*                         p_fn,
            ::Das::ExportInterface::IDasVariantVector*  p_args,
            ::Das::ExportInterface::IDasVariantVector** pp_out) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    // Mock IDasComponentFactory with configurable supported IIDs
    class MockFactory final : public IDasComponentFactory
    {
        std::atomic<uint32_t> ref_{0};

    public:
        std::unordered_map<DasGuid, DasResult> supported_iids;
        int                                    is_supported_call_count = 0;
        int                                    create_instance_call_count = 0;

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_; }
        uint32_t DAS_STD_CALL Release() override
        {
            auto c = --ref_;
            if (c == 0)
            {
                delete this;
            }
            return c;
        }
        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp) override
        {
            if (!pp)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp = this;
            AddRef();
            return DAS_S_OK;
        }
        DasResult DAS_STD_CALL GetGuid(DasGuid* p) override
        {
            if (p)
            {
                *p = DasGuid{};
            }
            return DAS_S_OK;
        }
        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
        DasResult IsSupported(const DasGuid& component_iid) override
        {
            ++is_supported_call_count;
            auto it = supported_iids.find(component_iid);
            if (it != supported_iids.end())
            {
                return it->second;
            }
            return DAS_E_NOT_FOUND;
        }
        DasResult CreateInstance(const DasGuid&, IDasComponent** pp_out)
            override
        {
            if (!pp_out)
            {
                return DAS_E_INVALID_POINTER;
            }
            ++create_instance_call_count;
            auto* comp = new MockComponent();
            comp->AddRef();
            *pp_out = comp;
            return DAS_S_OK;
        }
    };

    DasGuid MakeTestGuid(uint32_t v)
    {
        DasGuid guid{};
        guid.data1 = v;
        return guid;
    }

    class ComponentFactoryManagerTest : public ::testing::Test
    {
    protected:
        ComponentFactoryManager mgr;

        FeatureInfo MakeFeature(MockFactory& factory)
        {
            FeatureInfo feat{};
            feat.feature_type = DAS_PLUGIN_FEATURE_COMPONENT_FACTORY;
            feat.interface_ptr =
                DasPtr<IDasBase>(static_cast<IDasBase*>(&factory));
            return feat;
        }
    };

    TEST_F(ComponentFactoryManagerTest, OnPluginLoadedRegistersFactory)
    {
        auto* f = new MockFactory();
        f->AddRef();
        f->supported_iids[MakeTestGuid(42)] = DAS_S_OK;

        auto         feat = MakeFeature(*f);
        auto         guid = MakeTestGuid(1);
        FeatureInfo* fp = &feat;
        ASSERT_EQ(mgr.OnPluginLoaded(guid, {&fp, 1}), DAS_S_OK);

        DasPtr<IDasComponent> comp;
        EXPECT_EQ(mgr.CreateComponent(MakeTestGuid(42), comp.Put()), DAS_S_OK);
        EXPECT_NE(comp.Get(), nullptr);

        f->Release();
    }

    TEST_F(ComponentFactoryManagerTest, OnPluginLoadedZeroFeaturesIsNoop)
    {
        EXPECT_EQ(mgr.OnPluginLoaded(MakeTestGuid(1), {}), DAS_S_OK);
    }

    TEST_F(ComponentFactoryManagerTest, CachedIIDReturnsOkWithoutReprobe)
    {
        auto* f = new MockFactory();
        f->AddRef();
        f->supported_iids[MakeTestGuid(42)] = DAS_S_OK;

        auto         feat = MakeFeature(*f);
        auto         guid = MakeTestGuid(1);
        FeatureInfo* fp = &feat;
        mgr.OnPluginLoaded(guid, {&fp, 1});

        DasPtr<IDasComponent> c1, c2;
        ASSERT_EQ(mgr.CreateComponent(MakeTestGuid(42), c1.Put()), DAS_S_OK);
        ASSERT_EQ(mgr.CreateComponent(MakeTestGuid(42), c2.Put()), DAS_S_OK);
        EXPECT_EQ(f->is_supported_call_count, 1);
        EXPECT_EQ(f->create_instance_call_count, 2);

        f->Release();
    }

    TEST_F(ComponentFactoryManagerTest, UncachedIIDProbesAndCaches)
    {
        auto* f = new MockFactory();
        f->AddRef();
        f->supported_iids[MakeTestGuid(99)] = DAS_S_OK;

        auto         feat = MakeFeature(*f);
        auto         guid = MakeTestGuid(1);
        FeatureInfo* fp = &feat;
        mgr.OnPluginLoaded(guid, {&fp, 1});

        DasPtr<IDasComponent> c1;
        ASSERT_EQ(mgr.CreateComponent(MakeTestGuid(99), c1.Put()), DAS_S_OK);
        EXPECT_EQ(f->is_supported_call_count, 1);

        DasPtr<IDasComponent> c2;
        ASSERT_EQ(mgr.CreateComponent(MakeTestGuid(99), c2.Put()), DAS_S_OK);
        EXPECT_EQ(f->is_supported_call_count, 1);

        f->Release();
    }

    TEST_F(ComponentFactoryManagerTest, UnsupportedIIDReturnsNotFound)
    {
        auto* f = new MockFactory();
        f->AddRef();

        auto         feat = MakeFeature(*f);
        auto         guid = MakeTestGuid(1);
        FeatureInfo* fp = &feat;
        mgr.OnPluginLoaded(guid, {&fp, 1});

        DasPtr<IDasComponent> comp;
        EXPECT_EQ(
            mgr.CreateComponent(MakeTestGuid(999), comp.Put()),
            DAS_E_NOT_FOUND);

        f->Release();
    }

    TEST_F(ComponentFactoryManagerTest, OnPluginUnloadingClearsRoutingTable)
    {
        auto* f = new MockFactory();
        f->AddRef();
        f->supported_iids[MakeTestGuid(42)] = DAS_S_OK;

        auto         feat = MakeFeature(*f);
        auto         guid = MakeTestGuid(1);
        FeatureInfo* fp = &feat;
        mgr.OnPluginLoaded(guid, {&fp, 1});

        DasPtr<IDasComponent> c;
        ASSERT_EQ(mgr.CreateComponent(MakeTestGuid(42), c.Put()), DAS_S_OK);

        mgr.OnPluginUnloading(guid);

        DasPtr<IDasComponent> c2;
        EXPECT_EQ(
            mgr.CreateComponent(MakeTestGuid(42), c2.Put()),
            DAS_E_NOT_FOUND);

        f->Release();
    }

    TEST_F(ComponentFactoryManagerTest, NullOutParamReturnsInvalidPointer)
    {
        EXPECT_EQ(
            mgr.CreateComponent(MakeTestGuid(42), nullptr),
            DAS_E_INVALID_POINTER);
    }

    TEST_F(ComponentFactoryManagerTest, ConcurrentCreateComponent)
    {
        auto* f = new MockFactory();
        f->AddRef();
        f->supported_iids[MakeTestGuid(42)] = DAS_S_OK;

        auto         feat = MakeFeature(*f);
        auto         guid = MakeTestGuid(1);
        FeatureInfo* fp = &feat;
        mgr.OnPluginLoaded(guid, {&fp, 1});

        std::vector<std::thread> threads;
        std::atomic<int>         ok{0};
        for (int i = 0; i < 8; ++i)
        {
            threads.emplace_back(
                [&]()
                {
                    DasPtr<IDasComponent> c;
                    if (mgr.CreateComponent(MakeTestGuid(42), c.Put())
                        == DAS_S_OK)
                    {
                        ++ok;
                    }
                });
        }
        for (auto& t : threads)
        {
            t.join();
        }
        EXPECT_EQ(ok, 8);

        f->Release();
    }

    TEST_F(ComponentFactoryManagerTest, UnloadingUnknownGuidIsNoop)
    {
        EXPECT_EQ(mgr.OnPluginUnloading(MakeTestGuid(999)), DAS_S_OK);
    }
} // anonymous namespace
