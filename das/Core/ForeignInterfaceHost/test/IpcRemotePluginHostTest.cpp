#include <das/Core/ForeignInterfaceHost/NativeIpcRuntime.h>
#include <das/Core/ForeignInterfaceHost/RemotePluginHost.h>

#include <boost/asio/io_context.hpp>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/DasSharedRef.hpp>
#include <das/DasString.hpp>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <gtest/gtest.h>
#include <tl/expected.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace DAS::Core::ForeignInterfaceHost;

namespace
{
    DasGuid MakeRemotePluginHostGuid(uint32_t value)
    {
        DasGuid guid{};
        guid.data1 = value;
        return guid;
    }

    DAS::DasPtr<IDasReadOnlyString> MakeReadOnlyString(const char* value)
    {
        DAS::DasPtr<IDasReadOnlyString> result;
        EXPECT_EQ(CreateIDasReadOnlyStringFromUtf8(value, result.Put()), DAS_S_OK);
        return result;
    }

    class FakeBaseObject final : public IDasBase
    {
    public:
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
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (!pp_out_object)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DAS_IID_BASE)
            {
                *pp_out_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

    private:
        std::atomic<uint32_t> ref_count_{0};
    };

    class FakeLoadPluginOperation final : public IDasAsyncLoadPluginOperation
    {
    public:
        explicit FakeLoadPluginOperation(DAS::DasPtr<IDasBase> object)
            : object_{std::move(object)}
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
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (!pp_out_object)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DAS_IID_ASYNC_LOAD_PLUGIN_OPERATION
                || iid == DAS_IID_ASYNC_OPERATION || iid == DAS_IID_BASE)
            {
                *pp_out_object =
                    static_cast<IDasAsyncLoadPluginOperation*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        int32_t DAS_STD_CALL GetStatus() override { return status_; }

        DasResult DAS_STD_CALL
        SetCompleted(IDasAsyncCompletedHandler* p_handler) override
        {
            handler_ = p_handler;
            if (handler_)
            {
                handler_->OnCompleted(this, status_);
            }
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL Cancel() override
        {
            status_ = DAS_ASYNC_CANCELED;
            result_ = DAS_E_FAIL;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL GetResults(IDasBase** pp_out_plugin) override
        {
            if (!pp_out_plugin)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_plugin = nullptr;
            if (DAS::IsFailed(result_))
            {
                return result_;
            }
            if (object_)
            {
                object_->AddRef();
                *pp_out_plugin = object_.Get();
            }
            return result_;
        }

        void Fail(DasResult result)
        {
            result_ = result;
            status_ = DAS_ASYNC_FAILED;
        }

    private:
        std::atomic<uint32_t> ref_count_{0};
        int32_t               status_ = DAS_ASYNC_COMPLETED;
        DasResult             result_ = DAS_S_OK;
        DAS::DasPtr<IDasBase> object_;
        DAS::DasPtr<IDasAsyncCompletedHandler> handler_;
    };

    class FakeHostLauncher final : public DAS::Core::IPC::IHostLauncher
    {
    public:
        uint32_t AddRef() override { return ++ref_count_; }

        uint32_t Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult QueryInterface(const DasGuid& iid, void** pp) override
        {
            if (!pp)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DAS_IID_BASE)
            {
                *pp = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult StartAsync(
            const std::string&,
            IDasAsyncHandshakeOperation**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult Start(
            const std::string&,
            uint16_t&,
            uint32_t) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult StartWithDesc(
            const DAS::Core::IPC::HostLaunchDesc* p_desc,
            uint32_t                              timeout_ms,
            uint16_t* p_out_session_id) override
        {
            ++start_with_desc_count;
            observed_timeout_ms = timeout_ms;
            if (!p_desc || !p_out_session_id)
            {
                return DAS_E_INVALID_ARGUMENT;
            }

            const char* executable = nullptr;
            if (!p_desc->p_executable_path
                || DAS::IsFailed(p_desc->p_executable_path->GetUtf8(&executable)))
            {
                return DAS_E_INVALID_ARGUMENT;
            }
            observed_executable = executable;
            observed_arg_count = p_desc->arg_count;

            *p_out_session_id = session_to_return;
            session_id = session_to_return;
            running = DAS::IsOk(start_result);
            return start_result;
        }

        void Stop() override
        {
            ++stop_count;
            running = false;
            session_id = 0;
        }

        [[nodiscard]]
        bool IsRunning() const override
        {
            return running;
        }

        [[nodiscard]]
        uint32_t GetPid() const override
        {
            return 1234;
        }

        [[nodiscard]]
        uint16_t GetSessionId() const override
        {
            return session_id;
        }

        DasResult start_result = DAS_S_OK;
        uint16_t  session_to_return = 77;
        int       start_with_desc_count = 0;
        int       stop_count = 0;
        uint32_t  observed_timeout_ms = 0;
        uint16_t  session_id = 0;
        size_t    observed_arg_count = 0;
        bool      running = false;
        std::string observed_executable;

    private:
        std::atomic<uint32_t> ref_count_{0};
    };

    class FakeIpcContext final
        : public DAS::Core::IPC::MainProcess::IIpcContext
    {
    public:
        explicit FakeIpcContext(DAS::DasPtr<FakeHostLauncher> launcher)
            : launcher_{std::move(launcher)}
        {
            auto* object = new FakeBaseObject();
            object->AddRef();
            load_operation_ = DAS::DasPtr<FakeLoadPluginOperation>(
                new FakeLoadPluginOperation(
                    DAS::DasPtr<IDasBase>::Attach(object)));
        }

        DasResult CreateHostLauncher(
            DAS::Core::IPC::IHostLauncher** pp_out_launcher) override
        {
            ++create_launcher_count;
            if (!pp_out_launcher)
            {
                return DAS_E_INVALID_ARGUMENT;
            }
            if (DAS::IsFailed(create_launcher_result))
            {
                return create_launcher_result;
            }
            *pp_out_launcher = launcher_.Get();
            return DAS_S_OK;
        }

        DasResult LoadPluginAsync(
            DAS::Core::IPC::IHostLauncher* host_launcher,
            const char*                    u8_plugin_path,
            IDasAsyncLoadPluginOperation** pp_out_operation,
            std::chrono::milliseconds timeout =
                std::chrono::seconds(30)) override
        {
            ++load_async_count;
            observed_launcher = host_launcher;
            observed_plugin_path = u8_plugin_path ? u8_plugin_path : "";
            observed_load_timeout = timeout;
            if (DAS::IsFailed(load_async_result))
            {
                return load_async_result;
            }
            load_operation_->AddRef();
            *pp_out_operation = load_operation_.Get();
            return DAS_S_OK;
        }

        void PostCallback(IDasAsyncCallback* callback) override
        {
            if (callback)
            {
                callback->Do();
            }
        }

        void PostToBusinessThread(IDasAsyncCallback* callback) override
        {
            if (callback)
            {
                callback->Do();
            }
        }

        DasResult Run() override { return DAS_S_OK; }
        void RequestStop() override {}
        boost::asio::io_context& GetIoContext() override { return io_context_; }
        uint16_t AllocateSessionId() override { return 2; }
        void ReleaseSessionId(uint16_t) override {}

        DasResult CreateRemoteProxy(
            DAS::Core::IPC::ObjectId,
            const DasGuid&,
            IDasBase**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult ResolveMainProcessInterface(
            const DasGuid&,
            IDasBase**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult RegisterService(IDasBase*, const DasGuid&) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult UnregisterService(const DasGuid&) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult ResolveMainProcessInterfaceByName(
            const char*,
            IDasBase**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult RegisterServiceByName(
            IDasBase*,
            const DasGuid&,
            const char*) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult UnregisterServiceByName(const char*) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS::DasPtr<FakeHostLauncher> launcher_;
        DAS::DasPtr<FakeLoadPluginOperation> load_operation_;
        boost::asio::io_context io_context_;
        DasResult create_launcher_result = DAS_S_OK;
        DasResult load_async_result = DAS_S_OK;
        int       create_launcher_count = 0;
        int       load_async_count = 0;
        DAS::Core::IPC::IHostLauncher* observed_launcher = nullptr;
        std::string observed_plugin_path;
        std::chrono::milliseconds observed_load_timeout{0};
    };

    RemotePluginLoadRequest MakeRequest(
        DAS::DasPtr<IDasReadOnlyString>& executable)
    {
        executable = MakeReadOnlyString("DasHost.exe");

        RemotePluginLoadRequest request{};
        request.launch_desc.p_executable_path = executable.Get();
        request.manifest_path =
            std::filesystem::path{"plugins/TestPlugin.json"};
        request.plugin_guid = MakeRemotePluginHostGuid(0x75050003);
        request.timeout = std::chrono::milliseconds{4321};
        return request;
    }

    class CapturingRemotePluginHost final : public IRemotePluginHost
    {
    public:
        auto LoadPlugin(const RemotePluginLoadRequest& request)
            -> DAS::Utils::Expected<RuntimeLoadResult> override
        {
            ++load_count;
            observed_manifest_path = request.manifest_path;
            observed_plugin_guid = request.plugin_guid;
            observed_timeout = request.timeout;
            observed_arg_count = request.launch_desc.arg_count;

            const char* executable = nullptr;
            if (request.launch_desc.p_executable_path)
            {
                request.launch_desc.p_executable_path->GetUtf8(&executable);
            }
            observed_executable = executable ? executable : "";

            observed_args.clear();
            for (size_t i = 0; i < request.launch_desc.arg_count; ++i)
            {
                const char* arg = nullptr;
                request.launch_desc.pp_args[i]->GetUtf8(&arg);
                observed_args.emplace_back(arg ? arg : "");
            }

            if (DAS::IsFailed(load_error))
            {
                return tl::make_unexpected(load_error);
            }

            auto* object = new FakeBaseObject();
            object->AddRef();
            RuntimeLoadResult result{};
            result.object = DAS::DasPtr<IDasBase>::Attach(object);
            result.owner_session_id = owner_session_id;
            return result;
        }

        int load_count = 0;
        uint16_t owner_session_id = 88;
        DasResult load_error = DAS_S_OK;
        std::filesystem::path observed_manifest_path;
        DasGuid observed_plugin_guid{};
        std::chrono::milliseconds observed_timeout{0};
        size_t observed_arg_count = 0;
        std::string observed_executable;
        std::vector<std::string> observed_args;
    };
} // namespace

TEST(IpcRemotePluginHostContract, RequestUsesHostLaunchDescAndRuntimeResult)
{
    static_assert(std::is_abstract_v<IRemotePluginHost>);
    static_assert(std::is_same_v<
                  decltype(RemotePluginLoadRequest::launch_desc),
                  DAS::Core::IPC::HostLaunchDesc>);
    static_assert(std::is_same_v<
                  decltype(std::declval<IRemotePluginHost&>().LoadPlugin(
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

TEST(IpcRemotePluginHostLifecycle, LoadStartsHostAndReturnsOwnerSession)
{
    auto launcher = DAS::DasPtr<FakeHostLauncher>(
        new FakeHostLauncher());
    auto context = std::make_shared<FakeIpcContext>(launcher);
    IpcRemotePluginHost host{
        DAS::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(context)};

    DAS::DasPtr<IDasReadOnlyString> executable;
    auto request = MakeRequest(executable);

    auto result = host.LoadPlugin(request);

    ASSERT_TRUE(result);
    EXPECT_NE(result->object.Get(), nullptr);
    EXPECT_EQ(result->owner_session_id, launcher->session_to_return);
    EXPECT_EQ(context->create_launcher_count, 1);
    EXPECT_EQ(launcher->start_with_desc_count, 1);
    EXPECT_EQ(launcher->observed_executable, "DasHost.exe");
    EXPECT_EQ(launcher->observed_timeout_ms, 4321u);
    EXPECT_EQ(context->load_async_count, 1);
    EXPECT_EQ(context->observed_launcher, launcher.Get());
    EXPECT_EQ(
        context->observed_plugin_path,
        std::filesystem::path{"plugins/TestPlugin.json"}.string());
    EXPECT_EQ(context->observed_load_timeout, std::chrono::milliseconds{4321});
    EXPECT_EQ(launcher->stop_count, 0);
}

TEST(IpcRemotePluginHostLifecycle, LoadFailureAfterStartStopsLauncher)
{
    auto launcher = DAS::DasPtr<FakeHostLauncher>(
        new FakeHostLauncher());
    auto context = std::make_shared<FakeIpcContext>(launcher);
    context->load_async_result = DAS_E_FILE_NOT_FOUND;
    IpcRemotePluginHost host{
        DAS::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>(context)};

    DAS::DasPtr<IDasReadOnlyString> executable;
    auto request = MakeRequest(executable);

    auto result = host.LoadPlugin(request);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), DAS_E_FILE_NOT_FOUND);
    EXPECT_EQ(launcher->start_with_desc_count, 1);
    EXPECT_EQ(launcher->stop_count, 1);
}

TEST(NativeIpcRuntime, DelegatesDasHostLaunchToRemotePluginHost)
{
    auto remote = std::make_unique<CapturingRemotePluginHost>();
    auto* raw_remote = remote.get();
    NativeIpcRuntime runtime{
        std::filesystem::path{"DasHost.exe"},
        std::move(remote),
        std::chrono::milliseconds{2468}};

    RuntimeLoadRequest request{};
    request.manifest_path = std::filesystem::path{"plugins/Native.json"};
    request.plugin_guid = MakeRemotePluginHostGuid(0x75050004);

    auto result = runtime.LoadPlugin(request);

    ASSERT_TRUE(result);
    EXPECT_NE(result->object.Get(), nullptr);
    EXPECT_EQ(result->owner_session_id, raw_remote->owner_session_id);
    EXPECT_EQ(raw_remote->load_count, 1);
    EXPECT_EQ(
        raw_remote->observed_manifest_path,
        std::filesystem::path{"plugins/Native.json"});
    EXPECT_EQ(raw_remote->observed_plugin_guid.data1, 0x75050004u);
    EXPECT_EQ(raw_remote->observed_timeout, std::chrono::milliseconds{2468});
    EXPECT_EQ(raw_remote->observed_executable, "DasHost.exe");
    ASSERT_EQ(raw_remote->observed_args.size(), 2u);
    EXPECT_EQ(raw_remote->observed_args[0], "--main-pid");
    EXPECT_FALSE(raw_remote->observed_args[1].empty());
}

TEST(NativeIpcRuntime, FactoryCreatesNativeProvider)
{
    auto remote = std::make_unique<CapturingRemotePluginHost>();
    auto provider = CreateNativeIpcRuntimeProvider(
        std::filesystem::path{"DasHost.exe"},
        std::move(remote));

    ASSERT_TRUE(provider);
    RuntimeLoadRequest request{};
    request.manifest_path = std::filesystem::path{"plugins/Native.json"};
    request.plugin_guid = MakeRemotePluginHostGuid(0x75050005);

    auto result = provider.value()->LoadPlugin(request);

    ASSERT_TRUE(result);
    EXPECT_EQ(result->owner_session_id, 88);
}
