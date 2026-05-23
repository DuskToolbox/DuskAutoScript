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
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <type_traits>

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

} // namespace Das::IPC::Test
