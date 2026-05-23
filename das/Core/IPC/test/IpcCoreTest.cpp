/**
 * @file IpcCoreTest.cpp
 * @brief IPC Core Test Suite Entry Point
 *
 * This file provides the main test entry point and common test utilities
 * for the IPC (Inter-Process Communication) module.
 *
 * Test Matrix Reference (B9):
 * - IpcSerializerTest: Protocol rules / negative cases / boundary
 * - IpcObjectManagerTest: generation / ValidateHandle / IsLocal / stale handle
 * - IpcSharedMemoryPoolTest: Allocate / Deallocate / CleanupStaleBlocks
 * - IpcMessageQueueTransportTest: Small messages / Large messages (>4KB)
 * - IpcConnectionManagerTest: Heartbeat timeout / CleanupConnectionResources
 * - IpcRunLoopTest: Re-entrant / concurrent / timeout / cancel / >32 reject
 */

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HandshakeSerialization.h>
#include <das/Core/IPC/Host/HostCommandHandlers.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <type_traits>
#include <unordered_map>

namespace Das::IPC::Test
{

    // ====== IPC Test Environment ======

    /**
     * @brief Common test environment for IPC tests
     *
     * Provides shared setup/teardown for IPC-related tests.
     */
    class IpcCoreTestEnvironment : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            // Global IPC test initialization
            // Can be extended for platform-specific setup
        }

        void TearDown() override
        {
            // Global IPC test cleanup
        }
    };

    // ====== IPC Test Base Fixture ======

    /**
     * @brief Base test fixture for IPC tests
     *
     * Provides common utilities and helper methods for IPC tests.
     */
    class IpcCoreTestBase : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // Common setup for all IPC tests
        }

        void TearDown() override
        {
            // Common cleanup for all IPC tests
        }
    };

    class HostCommandHandlerTestPackage final
        : public Das::PluginInterface::IDasPluginPackage
    {
    public:
        uint32_t AddRef() override { return ++ref_count_; }

        uint32_t Release() override
        {
            const uint32_t next = --ref_count_;
            if (next == 0)
            {
                delete this;
            }
            return next;
        }

        DasResult QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            if (pp_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DAS_IID_BASE)
            {
                *pp_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid == DAS_IID_PLUGIN_PACKAGE)
            {
                *pp_object =
                    static_cast<Das::PluginInterface::IDasPluginPackage*>(
                        this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult EnumFeature(
            uint64_t,
            Das::PluginInterface::DasPluginFeature*) override
        {
            return DAS_E_OUT_OF_RANGE;
        }

        DasResult CreateFeatureInterface(uint64_t, IDasBase**) override
        {
            return DAS_E_OUT_OF_RANGE;
        }

        DasResult CanUnloadNow(bool* canUnloadNow) override
        {
            if (canUnloadNow == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *canUnloadNow = true;
            return DAS_S_OK;
        }

    private:
        std::atomic<uint32_t> ref_count_{1};
    };

    class FakeHostCommandContext final
        : public Das::Core::IPC::Host::IIpcContext
    {
    public:
        FakeHostCommandContext() { object_manager_.SetSessionId(42); }

        DasResult Run() override { return DAS_S_OK; }
        void      RequestStop() override {}

        bool IsConnected() const override { return true; }

        void RegisterCommandHandler(
            uint32_t                              cmd_type,
            Das::Core::IPC::Host::CommandHandler handler) override
        {
            handlers[cmd_type] = std::move(handler);
        }

        void PostCallback(IDasAsyncCallback*) override {}

        DasResult RegisterLocalObject(
            IDasBase*                  object_ptr,
            Das::Core::IPC::ObjectId& out_object_id) override
        {
            return object_manager_.RegisterLocalObject(
                object_ptr,
                out_object_id);
        }

        Das::Core::IPC::IDistributedObjectManager& GetObjectManager() override
        {
            return object_manager_;
        }

        DasResult ResolveMainProcessInterface(const DasGuid&, IDasBase**)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult ResolveMainProcessInterfaceByName(const char*, IDasBase**)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        std::unordered_map<uint32_t, Das::Core::IPC::Host::CommandHandler>
            handlers;

    private:
        Das::Core::IPC::DistributedObjectManager object_manager_;
    };

    // ====== Basic Connectivity Tests ======

    /**
     * @brief Test IPC core initialization
     */
    TEST_F(IpcCoreTestBase, CoreInitialization)
    {
        // Basic sanity check - IPC module should be loadable
        SUCCEED() << "IPC core test framework initialized successfully";
    }

    /**
     * @brief Test IPC test framework is working
     */
    TEST_F(IpcCoreTestBase, TestFrameworkWorking)
    {
        EXPECT_TRUE(true) << "Test framework is functional";
    }

    TEST_F(IpcCoreTestBase, HostIpcContextConfigRemainsCAbiFriendly)
    {
        using Das::Core::IPC::Host::IpcContextConfig;
        using Das::Core::IPC::Host::IpcContextEvents;

        EXPECT_TRUE(std::is_standard_layout_v<IpcContextEvents>);
        EXPECT_TRUE(std::is_trivially_copyable_v<IpcContextEvents>);
        EXPECT_TRUE(std::is_standard_layout_v<IpcContextConfig>);
        EXPECT_TRUE(std::is_trivially_copyable_v<IpcContextConfig>);

        IpcContextConfig config{};
        EXPECT_EQ(config.main_process_queue_name, nullptr);
        EXPECT_EQ(config.connect_url, nullptr);
        EXPECT_EQ(config.events.on_before_shutdown, nullptr);
        EXPECT_EQ(config.events.p_on_before_shutdown_context, nullptr);
    }

    TEST_F(IpcCoreTestBase, HostShutdownReasonUsesGeneratedEnumAbi)
    {
        using Das::Core::IPC::Host::HOST_SHUTDOWN_REASON_FORCE_DWORD;
        using Das::Core::IPC::Host::HOST_SHUTDOWN_REASON_GOODBYE;
        using Das::Core::IPC::Host::HOST_SHUTDOWN_REASON_PARENT_PROCESS_EXITED;
        using Das::Core::IPC::Host::HOST_SHUTDOWN_REASON_REQUEST_STOP;
        using Das::Core::IPC::Host::
            HOST_SHUTDOWN_REASON_TRANSPORT_DISCONNECTED;
        using Das::Core::IPC::Host::HostShutdownReason;
        using Das::Core::IPC::Host::OnBeforeShutdown;

        EXPECT_TRUE(std::is_enum_v<HostShutdownReason>);
        EXPECT_EQ(sizeof(HostShutdownReason), sizeof(uint32_t));

        uint32_t raw_reason = HOST_SHUTDOWN_REASON_GOODBYE;
        EXPECT_EQ(raw_reason, 0u);
        raw_reason = HOST_SHUTDOWN_REASON_TRANSPORT_DISCONNECTED;
        EXPECT_EQ(raw_reason, 1u);
        raw_reason = HOST_SHUTDOWN_REASON_PARENT_PROCESS_EXITED;
        EXPECT_EQ(raw_reason, 2u);
        raw_reason = HOST_SHUTDOWN_REASON_REQUEST_STOP;
        EXPECT_EQ(raw_reason, 3u);
        raw_reason = HOST_SHUTDOWN_REASON_FORCE_DWORD;
        EXPECT_EQ(raw_reason, 0x7FFFFFFFu);

        using ExpectedCallback = DasResult(DAS_STD_CALL*)(
            void*,
            HostShutdownReason,
            uint32_t);
        EXPECT_TRUE((std::is_same_v<OnBeforeShutdown, ExpectedCallback>));
    }

    TEST_F(IpcCoreTestBase, HostLaunchDescRemainsCAbiFriendly)
    {
        using Das::Core::IPC::HostLaunchDesc;
        using Das::Core::IPC::IHostLauncher;

        EXPECT_TRUE(std::is_standard_layout_v<HostLaunchDesc>);
        EXPECT_TRUE(std::is_trivially_copyable_v<HostLaunchDesc>);
        EXPECT_TRUE(
            (std::is_same_v<
                decltype(HostLaunchDesc::p_executable_path),
                IDasReadOnlyString*>));
        EXPECT_TRUE(
            (std::is_same_v<
                decltype(HostLaunchDesc::pp_args),
                IDasReadOnlyString* const*>));
        EXPECT_TRUE(
            (std::is_same_v<decltype(HostLaunchDesc::arg_count), size_t>));
        EXPECT_TRUE(
            (std::is_same_v<
                decltype(HostLaunchDesc::p_working_directory),
                IDasReadOnlyString*>));

        using StartWithDescSignature = DasResult (IHostLauncher::*)(
            const HostLaunchDesc*,
            uint32_t,
            uint16_t*);
        StartWithDescSignature method = &IHostLauncher::StartWithDesc;
        EXPECT_NE(method, nullptr);

        HostLaunchDesc desc{};
        EXPECT_EQ(desc.p_executable_path, nullptr);
        EXPECT_EQ(desc.pp_args, nullptr);
        EXPECT_EQ(desc.arg_count, 0u);
        EXPECT_EQ(desc.p_working_directory, nullptr);
    }

    TEST_F(IpcCoreTestBase, HostLaunchDescRejectsInvalidPointers)
    {
        boost::asio::io_context       io_context;
        Das::Core::IPC::HostLauncher launcher(io_context, 1, nullptr);
        uint16_t                     session_id = 0;

        EXPECT_EQ(
            launcher.StartWithDesc(nullptr, 1, &session_id),
            DAS_E_INVALID_ARGUMENT);

        Das::Core::IPC::HostLaunchDesc desc{};
        EXPECT_EQ(
            launcher.StartWithDesc(&desc, 1, &session_id),
            DAS_E_INVALID_ARGUMENT);
        EXPECT_EQ(
            launcher.StartWithDesc(&desc, 1, nullptr),
            DAS_E_INVALID_ARGUMENT);

        DAS::DasPtr<IDasReadOnlyString> executable;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8(
                "missing-host-executable",
                executable.Put()),
            DAS_S_OK);

        desc.p_executable_path = executable.Get();
        desc.arg_count = 1;
        desc.pp_args = nullptr;
        EXPECT_EQ(
            launcher.StartWithDesc(&desc, 1, &session_id),
            DAS_E_INVALID_ARGUMENT);

        IDasReadOnlyString* args[] = {nullptr};
        desc.pp_args = args;
        EXPECT_EQ(
            launcher.StartWithDesc(&desc, 1, &session_id),
            DAS_E_INVALID_ARGUMENT);
    }

    TEST_F(
        IpcCoreTestBase,
        HostCommandHandlersRegisterLoadPluginAndQueryInterface)
    {
        using Das::Core::IPC::IpcCommandResponse;
        using Das::Core::IPC::IpcCommandType;
        using Das::Core::IPC::ObjectId;
        using Das::Core::IPC::ValidatedIPCMessageHeader;
        using Das::Core::IPC::Host::HostCommandHandlerOptions;
        using Das::Core::IPC::Host::RegisterHostCommandHandlers;

        FakeHostCommandContext ctx;
        bool                   loader_called = false;
        const std::filesystem::path manifest_path{
            "C:/das/plugins/node-plugin/manifest.json"};

        HostCommandHandlerOptions options{};
        options.load_plugin =
            [&](const std::filesystem::path& path)
            -> Das::Utils::Expected<DAS::DasPtr<IDasBase>>
        {
            loader_called = true;
            EXPECT_EQ(path, manifest_path);
            return DAS::DasPtr<IDasBase>::Attach(
                static_cast<IDasBase*>(new HostCommandHandlerTestPackage));
        };

        ASSERT_EQ(RegisterHostCommandHandlers(&ctx, std::move(options)), DAS_S_OK);

        const auto load_command =
            static_cast<uint32_t>(IpcCommandType::LOAD_PLUGIN);
        const auto qi_command =
            static_cast<uint32_t>(IpcCommandType::QUERY_INTERFACE);
        ASSERT_TRUE(ctx.handlers.contains(load_command));
        ASSERT_TRUE(ctx.handlers.contains(qi_command));

        std::vector<uint8_t> load_payload;
        Das::Core::IPC::SerializeString(
            load_payload,
            manifest_path.string());

        IpcCommandResponse load_response{};
        ASSERT_EQ(
            ctx.handlers.at(load_command)(
                ValidatedIPCMessageHeader{},
                load_payload,
                load_response),
            DAS_S_OK);
        EXPECT_TRUE(loader_called);
        ASSERT_EQ(load_response.error_code, DAS_S_OK);
        ASSERT_EQ(
            load_response.response_data.size(),
            sizeof(ObjectId) + sizeof(DasGuid) + sizeof(uint16_t)
                + sizeof(uint16_t));

        ObjectId object_id{};
        std::memcpy(
            &object_id,
            load_response.response_data.data(),
            sizeof(object_id));
        EXPECT_EQ(object_id.session_id, 42u);
        EXPECT_NE(object_id.local_id, 0u);

        DasGuid load_iid{};
        std::memcpy(
            &load_iid,
            load_response.response_data.data() + sizeof(ObjectId),
            sizeof(load_iid));
        EXPECT_EQ(load_iid, DAS_IID_PLUGIN_PACKAGE);

        std::vector<uint8_t> qi_payload;
        qi_payload.insert(
            qi_payload.end(),
            reinterpret_cast<const uint8_t*>(&object_id),
            reinterpret_cast<const uint8_t*>(&object_id) + sizeof(object_id));
        qi_payload.insert(
            qi_payload.end(),
            reinterpret_cast<const uint8_t*>(&DAS_IID_PLUGIN_PACKAGE),
            reinterpret_cast<const uint8_t*>(&DAS_IID_PLUGIN_PACKAGE)
                + sizeof(DasGuid));

        IpcCommandResponse qi_response{};
        ASSERT_EQ(
            ctx.handlers.at(qi_command)(
                ValidatedIPCMessageHeader{},
                qi_payload,
                qi_response),
            DAS_S_OK);
        ASSERT_EQ(qi_response.error_code, DAS_S_OK);
        ASSERT_EQ(
            qi_response.response_data.size(),
            sizeof(int32_t) + sizeof(uint32_t) + sizeof(uint64_t));

        int32_t qi_result = DAS_E_FAIL;
        std::memcpy(
            &qi_result,
            qi_response.response_data.data(),
            sizeof(qi_result));
        EXPECT_EQ(qi_result, DAS_S_OK);
    }

} // namespace Das::IPC::Test
