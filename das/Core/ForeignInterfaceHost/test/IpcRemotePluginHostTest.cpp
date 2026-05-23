#include <das/Core/ForeignInterfaceHost/RemotePluginHost.h>

#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/DasString.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <type_traits>

using namespace DAS::Core::ForeignInterfaceHost;

namespace
{
    DasGuid MakeRemotePluginHostGuid(uint32_t value)
    {
        DasGuid guid{};
        guid.data1 = value;
        return guid;
    }
} // namespace

TEST(IpcRemotePluginHostContract, RequestUsesHostLaunchDescAndRuntimeResult)
{
    static_assert(std::is_abstract_v<IRemotePluginHost>);
    static_assert(std::is_same_v<
                  decltype(RemotePluginLoadRequest::launch_desc),
                  DAS::Core::IPC::HostLaunchDesc>);
    static_assert(std::is_same_v<
                  decltype(IRemotePluginHost::LoadPlugin(
                      std::declval<IRemotePluginHost&>(),
                      std::declval<const RemotePluginLoadRequest&>())),
                  DAS::Utils::Expected<RuntimeLoadResult>>);

    RemotePluginLoadRequest request{};
    request.manifest_path = std::filesystem::path{"plugins/TestPlugin.json"};
    request.plugin_guid = MakeRemotePluginHostGuid(0x75050001);
    request.timeout = std::chrono::milliseconds{1234};

    EXPECT_EQ(
        request.manifest_path,
        std::filesystem::path{"plugins/TestPlugin.json"});
    EXPECT_EQ(request.plugin_guid.data1, 0x75050001u);
    EXPECT_EQ(request.timeout, std::chrono::milliseconds{1234});
    EXPECT_EQ(request.launch_desc.p_executable_path, nullptr);
    EXPECT_EQ(request.launch_desc.pp_args, nullptr);
    EXPECT_EQ(request.launch_desc.arg_count, 0u);
    EXPECT_EQ(request.launch_desc.p_working_directory, nullptr);
}

TEST(IpcRemotePluginHostContract, LifecycleCallbacksAreOptionalHooks)
{
    bool process_exit_called = false;
    bool heartbeat_cleanup_called = false;

    RemotePluginLoadRequest request{};
    request.on_process_exit = [&](uint16_t session_id, int exit_code)
    {
        process_exit_called = true;
        EXPECT_EQ(session_id, 77);
        EXPECT_EQ(exit_code, 9);
    };
    request.on_heartbeat_timeout = [&](DasGuid guid)
    {
        heartbeat_cleanup_called = true;
        EXPECT_EQ(guid.data1, 0x75050002u);
    };

    request.on_process_exit(77, 9);
    request.on_heartbeat_timeout(MakeRemotePluginHostGuid(0x75050002));

    EXPECT_TRUE(process_exit_called);
    EXPECT_TRUE(heartbeat_cleanup_called);
}

