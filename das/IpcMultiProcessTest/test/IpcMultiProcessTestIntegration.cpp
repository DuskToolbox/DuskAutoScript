/**
 * @file IpcMultiProcessTestIntegration.cpp
 * @brief IPC 多进程集成测试 - 真正启动进程的测试
 *
 * 这些测试会启动真实的 DasHost.exe 进程进行端到端测试。
 * 需要 DAS_HOST_EXE_PATH 环境变量指向 DasHost.exe。
 *
 * 测试场景：
 * 1. 进程启动与关闭
 * 2. 握手协议（Hello -> Welcome -> Ready -> ReadyAck）
 * 3. IPC 连接验证
 */

#include <Das.ExportInterface.IDasVariantVector.hpp>
#include <Das.PluginInterface.IDasComponent.hpp>
#include <Das.PluginInterface.IDasPluginPackage.hpp>

#include "IpcMultiProcessTestIntegration.h"

#include "FakeMainProcess.h"
#include "IDasComponent.h"
#include "IDasPluginPackage.h"
#include "das/DasTypes.hpp"
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/IPC/AsyncOperationImpl.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/Utils/StdExecution.h>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <string_view>
#include <vector>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_PROCESS_WARNING

#include <boost/process/v2/environment.hpp>

DAS_DISABLE_WARNING_END

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <tlhelp32.h>
#include <windows.h>
#define DAS_IPC_TEST_CURRENT_PROCESS_ID() GetCurrentProcessId()
#else
#include <unistd.h>
#define DAS_IPC_TEST_CURRENT_PROCESS_ID() getpid()
#endif

using namespace Das::PluginInterface;
using namespace Das::ExportInterface;

namespace
{
    constexpr DasGuid IPC_TASK_GUID{
        0xA1B2C3D4,
        0xE5F6,
        0x4A7B,
        {0x8C, 0x9D, 0x0E, 0x1F, 0x2A, 0x3B, 0x4C, 0x5D}};

    constexpr DasGuid IPC_TASK_COMPONENT_GUID{
        0x68F10701,
        0x0000,
        0x4000,
        {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

    constexpr std::string_view DAS_MAAPI_TASK_GUID_TEXT =
        "69F20001-0000-4000-8000-000000000001";
    constexpr std::string_view DAS_MAAPI_AUTHORING_FACTORY_GUID_TEXT =
        "69F20002-0000-4000-8000-000000000001";

    yyjson::value ToYyjson(Das::ExportInterface::IDasJson* json)
    {
        DAS::DasPtr<IDasReadOnlyString> text;
        EXPECT_EQ(json->ToString(0, text.Put()), DAS_S_OK);
        const char* u8 = nullptr;
        EXPECT_EQ(text->GetUtf8(&u8), DAS_S_OK);
        auto parsed =
            Das::Utils::ParseYyjsonFromString(u8 != nullptr ? u8 : "");
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    class SessionOnlyHostLauncher final : public DAS::Core::IPC::IHostLauncher
    {
    public:
        SessionOnlyHostLauncher(uint16_t session_id, uint32_t pid)
            : session_id_(session_id), pid_(pid)
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override { return --ref_count_; }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            if (!pp_object)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_object = nullptr;
            if (iid == DasIidOf<IDasBase>())
            {
                AddRef();
                *pp_object = static_cast<IDasBase*>(
                    static_cast<DAS::Core::IPC::IHostLauncher*>(this));
                return DAS_S_OK;
            }
            return DAS_E_NO_INTERFACE;
        }

        DasResult StartAsync(const std::string&, IDasAsyncHandshakeOperation**)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult Start(const std::string&, uint16_t&, uint32_t) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult StartWithDesc(
            const DAS::Core::IPC::HostLaunchDesc*,
            uint32_t,
            uint16_t*) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        void Stop() override {}

        [[nodiscard]]
        bool IsRunning() const override
        {
            return true;
        }

        [[nodiscard]]
        uint32_t GetPid() const override
        {
            return pid_;
        }

        [[nodiscard]]
        uint16_t GetSessionId() const override
        {
            return session_id_;
        }

    private:
        uint16_t              session_id_ = 0;
        uint32_t              pid_ = 0;
        std::atomic<uint32_t> ref_count_{1};
    };

    class HostProcessGuard
    {
    public:
        HostProcessGuard(
            const std::string&              exe_path,
            const std::vector<std::string>& args)
        {
            process_.emplace(
                io_context_,
                exe_path,
                args,
                boost::process::v2::process_start_dir(
                    std::filesystem::path(exe_path).parent_path().string()));
        }

        ~HostProcessGuard() { Stop(); }

        HostProcessGuard(const HostProcessGuard&) = delete;
        HostProcessGuard& operator=(const HostProcessGuard&) = delete;

        uint32_t GetPid() const
        {
            return process_ ? static_cast<uint32_t>(process_->id())
                            : static_cast<uint32_t>(0);
        }

        bool IsRunning()
        {
            if (!process_)
            {
                return false;
            }
            boost::system::error_code ec;
            return process_->running(ec);
        }

        void Stop()
        {
            if (!process_)
            {
                return;
            }

            boost::system::error_code ec;
            if (process_->running(ec))
            {
                process_->terminate(ec);
            }
            process_.reset();
        }

    private:
        boost::asio::io_context                    io_context_;
        std::optional<boost::process::v2::process> process_;
    };

    class ScopedHostStop
    {
    public:
        explicit ScopedHostStop(
            DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher)
            : launcher_(std::move(launcher))
        {
        }

        ~ScopedHostStop()
        {
            if (launcher_ && launcher_->IsRunning())
            {
                launcher_->Stop();
            }
        }

        DAS::Core::IPC::IHostLauncher* Get() const { return launcher_.Get(); }

    private:
        DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher_;
    };

    struct SuspendedProcessForTest
    {
        uint32_t    pid = 0;
        bool        suspended = false;
        std::string failure;

#ifdef _WIN32
        std::vector<HANDLE> thread_handles;
#endif

        explicit operator bool() const noexcept { return suspended; }
    };

    SuspendedProcessForTest SuspendProcessForTest(uint32_t pid)
    {
        SuspendedProcessForTest result;
        result.pid = pid;
        if (pid == 0)
        {
            result.failure = "Cannot suspend process with pid 0";
            return result;
        }

#ifdef _WIN32
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            result.failure = DAS_FMT_NS::format(
                "CreateToolhelp32Snapshot failed: error={}",
                GetLastError());
            return result;
        }

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (!Thread32First(snapshot, &entry))
        {
            result.failure = DAS_FMT_NS::format(
                "Thread32First failed: error={}",
                GetLastError());
            CloseHandle(snapshot);
            return result;
        }

        do
        {
            if (entry.th32OwnerProcessID != pid)
            {
                continue;
            }

            HANDLE thread =
                OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (!thread)
            {
                continue;
            }

            if (SuspendThread(thread) == static_cast<DWORD>(-1))
            {
                CloseHandle(thread);
                continue;
            }

            result.thread_handles.push_back(thread);
        } while (Thread32Next(snapshot, &entry));

        CloseHandle(snapshot);

        if (result.thread_handles.empty())
        {
            result.failure = DAS_FMT_NS::format(
                "No threads could be suspended for pid={}",
                pid);
            return result;
        }
#else
        if (kill(static_cast<pid_t>(pid), SIGSTOP) != 0)
        {
            result.failure = DAS_FMT_NS::format(
                "SIGSTOP failed for pid={}: errno={}",
                pid,
                errno);
            return result;
        }
#endif

        result.suspended = true;
        return result;
    }

    void ResumeProcessForTest(SuspendedProcessForTest& process)
    {
        if (!process.suspended)
        {
            return;
        }

#ifdef _WIN32
        for (HANDLE thread : process.thread_handles)
        {
            if (!thread)
            {
                continue;
            }

            ResumeThread(thread);
            CloseHandle(thread);
        }
        process.thread_handles.clear();
#else
        if (process.pid != 0)
        {
            static_cast<void>(kill(static_cast<pid_t>(process.pid), SIGCONT));
        }
#endif

        process.suspended = false;
    }

    class ScopedProcessResumeForTest
    {
    public:
        explicit ScopedProcessResumeForTest(SuspendedProcessForTest& process)
            : process_(&process)
        {
        }

        ~ScopedProcessResumeForTest()
        {
            if (process_)
            {
                ResumeProcessForTest(*process_);
            }
        }

        ScopedProcessResumeForTest(const ScopedProcessResumeForTest&) = delete;
        ScopedProcessResumeForTest& operator=(
            const ScopedProcessResumeForTest&) = delete;

    private:
        SuspendedProcessForTest* process_ = nullptr;
    };

    DAS::DasPtr<IDasBase> LoadPluginPackageForHost(
        DAS::Core::IPC::MainProcess::IIpcContext& context,
        DAS::Core::IPC::IHostLauncher*            launcher,
        const std::string&                        plugin_path,
        size_t                                    attempts)
    {
        for (size_t attempt = 0; attempt < attempts; ++attempt)
        {
            DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
            DasResult result = context.LoadPluginAsync(
                launcher,
                plugin_path.c_str(),
                op.Put(),
                IpcTestConfig::GetPluginLoadTimeout());
            if (DAS::IsOk(result))
            {
                auto opt = DAS::Core::IPC::wait(
                    context,
                    DAS::Core::IPC::async_op(context, std::move(op)));
                if (opt.has_value())
                {
                    auto& [load_result, proxy] = *opt;
                    if (DAS::IsOk(load_result) && proxy)
                    {
                        return DAS::DasPtr<IDasBase>::Attach(proxy);
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return {};
    }

    DAS::DasPtr<IDasComponentFactory> GetComponentFactoryFromPackage(
        IDasBase* package_proxy,
        uint64_t  feature_index)
    {
        if (!package_proxy)
        {
            return {};
        }

        DAS::DasPtr<IDasBase> package_base(package_proxy);
        DasPluginPackage      plugin_package;
        if (DAS::IsFailed(package_base.As(plugin_package.Put())))
        {
            return {};
        }

        DAS::DasPtr<IDasBase> factory_base;
        if (DAS::IsFailed(plugin_package->CreateFeatureInterface(
                feature_index,
                factory_base.Put())))
        {
            return {};
        }

        DAS::DasPtr<IDasComponentFactory> factory;
        if (DAS::IsFailed(factory_base.As(factory.Put())))
        {
            return {};
        }

        return factory;
    }

    DAS::DasPtr<IDasComponent> CreateComponentFromFactory(
        IDasComponentFactory* factory)
    {
        if (!factory)
        {
            return {};
        }

        DAS::DasPtr<IDasComponent> component;
        if (DAS::IsFailed(factory->CreateInstance(
                DasIidOf<IDasComponent>(),
                component.Put())))
        {
            return {};
        }

        return component;
    }

    template <typename Predicate>
    bool WaitUntilForTest(
        Predicate&&               predicate,
        std::chrono::milliseconds timeout,
        std::chrono::milliseconds poll_interval =
            std::chrono::milliseconds(100))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
            {
                return true;
            }

            std::this_thread::sleep_for(poll_interval);
        }

        return predicate();
    }

    std::chrono::milliseconds HeartbeatTimeoutWindowForTest()
    {
        using DAS::Core::IPC::ConnectionManager;
        return std::chrono::milliseconds(
            ConnectionManager::HEARTBEAT_TIMEOUT_MS
            + (3 * ConnectionManager::HEARTBEAT_INTERVAL_MS));
    }

    void AssertQueryMainProcessStringIpcSucceeds(
        DAS::Core::IPC::MainProcess::IIpcContext& context,
        DAS::Core::IPC::IHostLauncher*            launcher,
        std::string_view                          expected_text)
    {
        ASSERT_NE(launcher, nullptr);

        std::string plugin_path;
        try
        {
            plugin_path =
                IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        }
        catch (const std::exception& e)
        {
            GTEST_SKIP() << "IpcTestPlugin1 JSON not found: " << e.what();
        }

        const std::string               expected_storage{expected_text};
        DAS::DasPtr<IDasReadOnlyString> service_string;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8(
                expected_storage.c_str(),
                service_string.Put()),
            DAS_S_OK);

        ASSERT_EQ(
            context.RegisterService(
                service_string.Get(),
                DasIidOf<IDasReadOnlyString>()),
            DAS_S_OK);

        auto package_proxy =
            LoadPluginPackageForHost(context, launcher, plugin_path, 3);
        ASSERT_NE(package_proxy.Get(), nullptr)
            << "IpcTestPlugin1 must load through IIpcContext::LoadPluginAsync";

        auto factory = GetComponentFactoryFromPackage(
            package_proxy.Get(),
            /*feature_index=*/1);
        ASSERT_NE(factory.Get(), nullptr);

        auto component = CreateComponentFromFactory(factory.Get());
        ASSERT_NE(component.Get(), nullptr);

        DasReadOnlyString method_name{"queryMainProcessString"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        const DasResult dispatch_hr = component->Dispatch(
            method_name.Get(),
            params.Get(),
            dispatch_result.Put());
        ASSERT_EQ(dispatch_hr, DAS_S_OK)
            << "Dispatch(queryMainProcessString) failed";

        ASSERT_NE(dispatch_result.Get(), nullptr);
        ASSERT_EQ(dispatch_result->GetSize(), 1u);

        DAS::DasPtr<IDasReadOnlyString> out_string;
        ASSERT_EQ(dispatch_result->GetString(0, out_string.Put()), DAS_S_OK);
        ASSERT_NE(out_string.Get(), nullptr);

        const char* out_text = nullptr;
        ASSERT_EQ(out_string->GetUtf8(&out_text), DAS_S_OK);
        ASSERT_NE(out_text, nullptr);
        EXPECT_STREQ(out_text, expected_storage.c_str());
    }

    std::filesystem::path ResolveNodeExecutableForIntegration()
    {
        constexpr auto NODE_HOST_EXECUTABLE_ENV = "DAS_NODE_HOST_EXE_PATH";
        if (const char* env = std::getenv(NODE_HOST_EXECUTABLE_ENV);
            env != nullptr && env[0] != '\0')
        {
            return std::filesystem::path{env};
        }

        const auto resolved =
            boost::process::v2::environment::find_executable("node");
        if (resolved.empty())
        {
            return {};
        }
        return std::filesystem::path{resolved.string()};
    }

    constexpr auto NODE_RUNTIME_PACKAGE_NAME = "das-core-node";
    constexpr auto NODE_HOST_SCRIPT_RELATIVE = "bin/das-node-host.cjs";
    constexpr auto NODE_RUNTIME_WRAPPER_FILE = "das_core_napi_export.js";
    constexpr auto NODE_RUNTIME_ADDON_RELATIVE = "native/das_core_napi.node";

#ifdef _WIN32
    constexpr auto DYNAMIC_LIBRARY_PATH_ENV = "PATH";
#elif defined(__APPLE__)
    constexpr auto DYNAMIC_LIBRARY_PATH_ENV = "DYLD_LIBRARY_PATH";
#else
    constexpr auto DYNAMIC_LIBRARY_PATH_ENV = "LD_LIBRARY_PATH";
#endif

    std::filesystem::path AppRootFromHostExePath(
        const std::string& host_exe_path)
    {
        const auto absolute_host =
            std::filesystem::absolute(std::filesystem::path{host_exe_path});
        return absolute_host.parent_path();
    }

    std::filesystem::path PluginCollectionRootFromHostExePath(
        const std::string& host_exe_path)
    {
        return AppRootFromHostExePath(host_exe_path) / "plugins";
    }

    std::filesystem::path NodeModulesRootFromHostExePath(
        const std::string& host_exe_path)
    {
        return PluginCollectionRootFromHostExePath(host_exe_path)
               / "node_modules";
    }

    std::filesystem::path NodePackageRootFromHostExePath(
        const std::string& host_exe_path)
    {
        return NodeModulesRootFromHostExePath(host_exe_path)
               / NODE_RUNTIME_PACKAGE_NAME;
    }

    bool IsAutogenNodePath(const std::filesystem::path& path)
    {
        const auto generic = path.generic_string();
        return generic.find("_autogen/idl/node") != std::string::npos;
    }

    bool HasNormalizedNodeHostPackage(const std::filesystem::path& package_root)
    {
        return std::filesystem::is_regular_file(package_root / "package.json")
               && std::filesystem::is_regular_file(package_root / "index.cjs")
               && std::filesystem::is_regular_file(
                   package_root / NODE_HOST_SCRIPT_RELATIVE)
               && std::filesystem::is_regular_file(
                   package_root / NODE_RUNTIME_WRAPPER_FILE)
               && std::filesystem::is_regular_file(
                   package_root / NODE_RUNTIME_ADDON_RELATIVE);
    }

    bool IsDasRuntimeLibraryArtifact(const std::filesystem::path& path)
    {
        const auto file_name = path.filename().string();
        const auto extension = path.extension().string();
        if (extension == ".dll" && file_name.rfind("DasCore", 0) == 0)
        {
            return true;
        }
        if (file_name.rfind("libDasCore", 0) == 0
            && (extension == ".dll" || extension == ".so"
                || extension == ".dylib"))
        {
            return true;
        }
        return false;
    }

    testing::AssertionResult AssertNoNodeRuntimeArtifactsInPluginRoot(
        const std::filesystem::path& plugin_root,
        bool                         allow_collection_node_modules)
    {
        const std::array<std::filesystem::path, 3> forbidden_files{
            "das-node-host.cjs",
            NODE_RUNTIME_WRAPPER_FILE,
            "das_core_napi.node"};

        for (const auto& relative : forbidden_files)
        {
            const auto candidate = plugin_root / relative;
            if (std::filesystem::exists(candidate))
            {
                return testing::AssertionFailure()
                       << "Node plugin root contains generated runtime artifact: "
                       << candidate.string();
            }
        }

        const auto node_modules = plugin_root / "node_modules";
        if (!allow_collection_node_modules
            && std::filesystem::exists(node_modules))
        {
            return testing::AssertionFailure()
                   << "Node plugin root contains plugin-local node_modules: "
                   << node_modules.string();
        }

        if (std::filesystem::is_directory(plugin_root))
        {
            for (const auto& entry :
                 std::filesystem::directory_iterator(plugin_root))
            {
                if (entry.is_regular_file()
                    && IsDasRuntimeLibraryArtifact(entry.path()))
                {
                    return testing::AssertionFailure()
                           << "Node plugin root contains DAS runtime library: "
                           << entry.path().string();
                }
            }
        }

        return testing::AssertionSuccess();
    }

    std::string EscapeJsonString(std::string_view value)
    {
        std::string result;
        for (const char ch : value)
        {
            switch (ch)
            {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += ch;
                break;
            }
        }
        return result;
    }

    std::string MakeNodeHostEnvironmentConfig(
        const std::filesystem::path& node_modules_root)
    {
        const auto app_root = node_modules_root.parent_path().parent_path();
        return std::string{R"({"version":1,"prepend":[{"name":")"}
               + DYNAMIC_LIBRARY_PATH_ENV + R"(","value":")"
               + EscapeJsonString(app_root.string()) + R"("}]})";
    }

    bool IsPluginCollectionRoot(
        const std::filesystem::path& plugin_root,
        const std::string&           host_exe_path)
    {
        std::error_code ec;
        const auto      expected = std::filesystem::weakly_canonical(
            PluginCollectionRootFromHostExePath(host_exe_path),
            ec);
        if (ec)
        {
            return false;
        }

        const auto actual = std::filesystem::weakly_canonical(plugin_root, ec);
        if (ec)
        {
            return false;
        }

        return actual == expected;
    }

    testing::AssertionResult ValidateNodeHostPackage(
        const std::filesystem::path& node_executable,
        const std::filesystem::path& package_root)
    {
        if (node_executable.empty()
            || !std::filesystem::is_regular_file(node_executable))
        {
            return testing::AssertionFailure() << "Node executable not found";
        }

        if (IsAutogenNodePath(package_root))
        {
            return testing::AssertionFailure()
                   << "Node host package must not resolve from _autogen/idl/node: "
                   << package_root.string();
        }

        if (package_root.filename() != NODE_RUNTIME_PACKAGE_NAME
            || package_root.parent_path().filename() != "node_modules")
        {
            return testing::AssertionFailure()
                   << "Node host package must be plugins/node_modules/"
                   << NODE_RUNTIME_PACKAGE_NAME
                   << ", got: " << package_root.string();
        }

        if (!HasNormalizedNodeHostPackage(package_root))
        {
            return testing::AssertionFailure()
                   << "Normalized Node host package missing from: "
                   << package_root.string();
        }

        return testing::AssertionSuccess();
    }

    DasResult StartNodeHostPackage(
        DAS::Core::IPC::IHostLauncher* launcher,
        const std::filesystem::path&   node_executable,
        const std::filesystem::path&   runtime_root,
        const std::filesystem::path&   package_root,
        const std::filesystem::path&   node_modules_root,
        uint16_t*                      session_id)
    {
        if (!launcher || !session_id)
        {
            return DAS_E_INVALID_POINTER;
        }

        const auto host_script = runtime_root / NODE_HOST_SCRIPT_RELATIVE;
        const auto executable_text = node_executable.string();
        const auto script_text = host_script.string();
        const auto main_pid_value = std::to_string(
            static_cast<uint32_t>(DAS_IPC_TEST_CURRENT_PROCESS_ID()));
        const auto working_directory_text = package_root.string();
        const auto package_root_text = package_root.string();
        const auto node_modules_root_text = node_modules_root.string();
        const auto environment_config_text =
            MakeNodeHostEnvironmentConfig(node_modules_root);

        DasReadOnlyString executable_string{executable_text.c_str()};
        DasReadOnlyString script_arg{script_text.c_str()};
        DasReadOnlyString main_pid_name{"--main-pid"};
        DasReadOnlyString main_pid_arg{main_pid_value.c_str()};
        DasReadOnlyString package_root_name{"--package-root"};
        DasReadOnlyString package_root_arg{package_root_text.c_str()};
        DasReadOnlyString node_modules_root_name{"--node-modules-root"};
        DasReadOnlyString node_modules_root_arg{node_modules_root_text.c_str()};
        DasReadOnlyString working_directory{working_directory_text.c_str()};
        DasReadOnlyString environment_config{environment_config_text.c_str()};

        std::array<IDasReadOnlyString*, 7> args{
            script_arg.Get(),
            main_pid_name.Get(),
            main_pid_arg.Get(),
            package_root_name.Get(),
            package_root_arg.Get(),
            node_modules_root_name.Get(),
            node_modules_root_arg.Get()};

        DAS::Core::IPC::HostLaunchDesc desc{};
        desc.p_executable_path = executable_string.Get();
        desc.pp_args = args.data();
        desc.arg_count = args.size();
        desc.p_working_directory = working_directory.Get();
        desc.p_environment_config = environment_config.Get();

        return launcher->StartWithDesc(
            &desc,
            IpcTestConfig::GetHostStartTimeoutMs(),
            session_id);
    }

    void AssertNodeComponentDispatchBehavior(IDasComponent* component)
    {
        ASSERT_NE(component, nullptr);

        {
            DasReadOnlyString method_name{"getSessionInfo"};
            DAS::ExportInterface::DasVariantVector dispatch_result;
            DAS::ExportInterface::DasVariantVector params;
            ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

            ASSERT_EQ(
                component->Dispatch(
                    method_name.Get(),
                    params.Get(),
                    dispatch_result.Put()),
                DAS_S_OK);
            ASSERT_NE(dispatch_result.Get(), nullptr);
            ASSERT_EQ(dispatch_result->GetSize(), 3u);

            int64_t session_value = 0;
            ASSERT_EQ(dispatch_result->GetInt(0, &session_value), DAS_S_OK);
            EXPECT_GE(session_value, 0);

            DAS::DasPtr<IDasReadOnlyString> language;
            ASSERT_EQ(dispatch_result->GetString(1, language.Put()), DAS_S_OK);
            const char* language_text = nullptr;
            ASSERT_EQ(language->GetUtf8(&language_text), DAS_S_OK);
            ASSERT_NE(language_text, nullptr);
            EXPECT_STREQ(language_text, "Node");

            DAS::DasPtr<IDasReadOnlyString> component_name;
            ASSERT_EQ(
                dispatch_result->GetString(2, component_name.Put()),
                DAS_S_OK);
            const char* component_name_text = nullptr;
            ASSERT_EQ(component_name->GetUtf8(&component_name_text), DAS_S_OK);
            ASSERT_NE(component_name_text, nullptr);
            EXPECT_STREQ(component_name_text, "NodeTestPlugin");
        }

        {
            DasReadOnlyString                      method_name{"echo"};
            DAS::ExportInterface::DasVariantVector dispatch_result;
            DAS::ExportInterface::DasVariantVector params;
            ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
            DasReadOnlyString input_text{"ipc-node-e2e"};
            ASSERT_EQ(params.Get()->PushBackString(input_text.Get()), DAS_S_OK);

            ASSERT_EQ(
                component->Dispatch(
                    method_name.Get(),
                    params.Get(),
                    dispatch_result.Put()),
                DAS_S_OK);
            ASSERT_NE(dispatch_result.Get(), nullptr);
            ASSERT_EQ(dispatch_result->GetSize(), 1u);

            DAS::DasPtr<IDasReadOnlyString> echo_value;
            ASSERT_EQ(
                dispatch_result->GetString(0, echo_value.Put()),
                DAS_S_OK);
            const char* echo_text = nullptr;
            ASSERT_EQ(echo_value->GetUtf8(&echo_text), DAS_S_OK);
            ASSERT_NE(echo_text, nullptr);
            EXPECT_STREQ(echo_text, "[Node] echo: ipc-node-e2e");
        }

        {
            DasReadOnlyString                      method_name{"compute"};
            DAS::ExportInterface::DasVariantVector dispatch_result;
            DAS::ExportInterface::DasVariantVector params;
            ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
            DasReadOnlyString operation{"mul"};
            ASSERT_EQ(params.Get()->PushBackString(operation.Get()), DAS_S_OK);
            ASSERT_EQ(params.Get()->PushBackInt(6), DAS_S_OK);
            ASSERT_EQ(params.Get()->PushBackInt(7), DAS_S_OK);

            ASSERT_EQ(
                component->Dispatch(
                    method_name.Get(),
                    params.Get(),
                    dispatch_result.Put()),
                DAS_S_OK);
            ASSERT_NE(dispatch_result.Get(), nullptr);
            ASSERT_EQ(dispatch_result->GetSize(), 1u);

            int64_t computed = 0;
            ASSERT_EQ(dispatch_result->GetInt(0, &computed), DAS_S_OK);
            EXPECT_EQ(computed, 42);
        }
    }

    testing::AssertionResult ValidatePythonRuntimeArtifacts(
        const std::string& host_exe_path)
    {
        const auto app_root = AppRootFromHostExePath(host_exe_path);
        const auto python_module = app_root / "DuskAutoScript.py";
        if (!std::filesystem::is_regular_file(python_module))
        {
            return testing::AssertionFailure()
                   << "Python runtime module not found at: "
                   << python_module.string();
        }

        bool has_native_export = false;
        bool needs_debug_python_abi = false;
        for (const auto& entry : std::filesystem::directory_iterator(app_root))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const auto filename = entry.path().filename().string();
            const auto extension = entry.path().extension().string();
            if (filename.find("DasCorePythonExport") != std::string::npos
                && (extension == ".pyd" || extension == ".dll"
                    || extension == ".so" || extension == ".dylib"))
            {
                has_native_export = true;
                needs_debug_python_abi =
                    filename.find("_d.") != std::string::npos
                    || (filename.size() >= 6
                        && filename.compare(filename.size() - 6, 6, "_d.pyd")
                               == 0);
                break;
            }
        }

        if (!has_native_export)
        {
            return testing::AssertionFailure()
                   << "Python native export artifact not found under: "
                   << app_root.string();
        }

#ifdef _WIN32
        const auto python_abi_runtime =
            app_root
            / (needs_debug_python_abi ? "python3_d.dll" : "python3.dll");
        if (!std::filesystem::is_regular_file(python_abi_runtime))
        {
            return testing::AssertionFailure()
                   << "Python ABI runtime DLL not copied beside DasHost.exe: "
                   << python_abi_runtime.string();
        }
#endif

        return testing::AssertionSuccess();
    }

    testing::AssertionResult ValidatePythonPluginPackageArtifacts(
        const std::string& plugin_json_path,
        std::string_view   plugin_source_name)
    {
        if (!std::filesystem::is_regular_file(plugin_json_path))
        {
            return testing::AssertionFailure()
                   << "Python plugin manifest not found at: "
                   << plugin_json_path;
        }

        const auto plugin_root =
            std::filesystem::path{plugin_json_path}.parent_path();
        const auto plugin_source =
            plugin_root / std::filesystem::path{plugin_source_name};
        if (!std::filesystem::is_regular_file(plugin_source))
        {
            return testing::AssertionFailure()
                   << "Python plugin source not found at: "
                   << plugin_source.string();
        }

        return testing::AssertionSuccess();
    }

    testing::AssertionResult AssertPathContainedInDirectory(
        const std::filesystem::path& child,
        const std::filesystem::path& parent)
    {
        std::error_code ec;
        const auto      canonical_parent =
            std::filesystem::weakly_canonical(parent, ec);
        if (ec)
        {
            return testing::AssertionFailure()
                   << "Could not canonicalize parent directory: "
                   << parent.string() << ", error=" << ec.message();
        }

        const auto canonical_child =
            std::filesystem::weakly_canonical(child, ec);
        if (ec)
        {
            return testing::AssertionFailure()
                   << "Could not canonicalize child path: " << child.string()
                   << ", error=" << ec.message();
        }

        const auto relative =
            std::filesystem::relative(canonical_child, canonical_parent, ec);
        if (ec || relative.empty() || relative.is_absolute())
        {
            return testing::AssertionFailure()
                   << "Path is not inside expected directory: child="
                   << canonical_child.string()
                   << ", parent=" << canonical_parent.string();
        }

        for (const auto& part : relative)
        {
            if (part == "..")
            {
                return testing::AssertionFailure()
                       << "Path escapes expected directory: child="
                       << canonical_child.string()
                       << ", parent=" << canonical_parent.string();
            }
        }

        return testing::AssertionSuccess();
    }

    testing::AssertionResult PreparePythonFolderTestPluginFixture(
        std::filesystem::path* out_manifest_path)
    {
        if (!out_manifest_path)
        {
            return testing::AssertionFailure()
                   << "out_manifest_path must not be null";
        }

        std::filesystem::path plugin_dir;
        try
        {
            plugin_dir = IpcTestConfig::GetPluginDir();
        }
        catch (const std::exception& e)
        {
            return testing::AssertionFailure()
                   << "DAS_PLUGIN_DIR not available: " << e.what();
        }

        if (!std::filesystem::is_directory(plugin_dir))
        {
            return testing::AssertionFailure()
                   << "DAS_PLUGIN_DIR is not a directory: "
                   << plugin_dir.string();
        }

        std::error_code ec;
        const auto      stale_root_manifest =
            plugin_dir / "PythonFolderTestPlugin.json";
        std::filesystem::remove(stale_root_manifest, ec);
        if (ec)
        {
            return testing::AssertionFailure()
                   << "Failed to remove stale Python folder root manifest: "
                   << stale_root_manifest.string()
                   << ", error=" << ec.message();
        }

        std::filesystem::path flat_manifest_path;
        try
        {
            flat_manifest_path =
                IpcTestConfig::GetTestPluginJsonPath("PythonTestPlugin");
        }
        catch (const std::exception& e)
        {
            return testing::AssertionFailure()
                   << "PythonTestPlugin manifest not found for folder fixture: "
                   << e.what();
        }

        const auto flat_root = flat_manifest_path.parent_path();
        const auto flat_source = flat_root / "python_test_plugin.py";
        if (!std::filesystem::is_regular_file(flat_source))
        {
            return testing::AssertionFailure()
                   << "PythonTestPlugin source not found for folder fixture: "
                   << flat_source.string();
        }

        const auto folder_root = plugin_dir / "PythonFolderTestPlugin";
        const auto containment =
            AssertPathContainedInDirectory(folder_root, plugin_dir);
        if (!containment)
        {
            return containment;
        }

        std::filesystem::remove_all(folder_root, ec);
        if (ec)
        {
            return testing::AssertionFailure()
                   << "Failed to remove Python folder fixture: "
                   << folder_root.string() << ", error=" << ec.message();
        }

        std::filesystem::create_directories(folder_root, ec);
        if (ec)
        {
            return testing::AssertionFailure()
                   << "Failed to create Python folder fixture: "
                   << folder_root.string() << ", error=" << ec.message();
        }

        const auto folder_source = folder_root / "python_folder_test_plugin.py";
        std::filesystem::copy_file(
            flat_source,
            folder_source,
            std::filesystem::copy_options::overwrite_existing,
            ec);
        if (ec)
        {
            return testing::AssertionFailure()
                   << "Failed to copy Python folder fixture source: "
                   << folder_source.string() << ", error=" << ec.message();
        }

        const auto    manifest_path = folder_root / "manifest.json";
        std::ofstream manifest{manifest_path, std::ios::binary};
        if (!manifest)
        {
            return testing::AssertionFailure()
                   << "Failed to open Python folder fixture manifest: "
                   << manifest_path.string();
        }

        manifest << "{\n"
                 << "    \"name\": \"PythonFolderTestPlugin\",\n"
                 << "    \"author\": \"Dusk\",\n"
                 << "    \"version\": \"0.1\",\n"
                 << "    \"guid\": \"5E6F7081-9ABC-4E6F-B102-4C5D6E7F8092\",\n"
                 << "    \"description\": \"Python Folder Test Plugin for IPC "
                    "integration testing\",\n"
                 << "    \"supportedSystem\": \"Windows\",\n"
                 << "    \"language\": \"Python\",\n"
                 << "    \"pluginFilenameExtension\": \"py\",\n"
                 << "    \"entryPoint\": "
                    "\"python_folder_test_plugin.create_plugin\",\n"
                 << "    \"settings\": []\n"
                 << "}\n";
        manifest.close();
        if (!manifest)
        {
            return testing::AssertionFailure()
                   << "Failed to write Python folder fixture manifest: "
                   << manifest_path.string();
        }

        *out_manifest_path = manifest_path;
        return testing::AssertionSuccess();
    }

    void AssertPythonFactoryRejectsUnsupportedIid(IDasComponentFactory* factory)
    {
        ASSERT_NE(factory, nullptr);

        DasGuid unsupported_component_iid{};
        EXPECT_EQ(
            factory->IsSupported(unsupported_component_iid),
            DAS_E_NO_IMPLEMENTATION)
            << "Python component factory must reject unsupported IIDs";

        IDasComponent* unsupported_component_raw = nullptr;
        EXPECT_EQ(
            factory->CreateInstance(
                unsupported_component_iid,
                &unsupported_component_raw),
            DAS_E_NO_IMPLEMENTATION)
            << "Python component factory must not create unsupported "
               "component interfaces";
        EXPECT_EQ(unsupported_component_raw, nullptr);
    }

    void AssertPythonComponentDispatchBehavior(
        IDasComponent*   component,
        std::string_view expected_component_name)
    {
        ASSERT_NE(component, nullptr);

        {
            DasReadOnlyString method_name{"getSessionInfo"};
            DAS::ExportInterface::DasVariantVector dispatch_result;
            DAS::ExportInterface::DasVariantVector params;
            ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

            ASSERT_EQ(
                component->Dispatch(
                    method_name.Get(),
                    params.Get(),
                    dispatch_result.Put()),
                DAS_S_OK);
            ASSERT_NE(dispatch_result.Get(), nullptr);
            ASSERT_EQ(dispatch_result->GetSize(), 3u);

            int64_t session_value = 0;
            ASSERT_EQ(dispatch_result->GetInt(0, &session_value), DAS_S_OK);
            EXPECT_GE(session_value, 0);

            DAS::DasPtr<IDasReadOnlyString> language;
            ASSERT_EQ(dispatch_result->GetString(1, language.Put()), DAS_S_OK);
            const char* language_text = nullptr;
            ASSERT_EQ(language->GetUtf8(&language_text), DAS_S_OK);
            ASSERT_NE(language_text, nullptr);
            EXPECT_STREQ(language_text, "Python");

            DAS::DasPtr<IDasReadOnlyString> component_name;
            ASSERT_EQ(
                dispatch_result->GetString(2, component_name.Put()),
                DAS_S_OK);
            const char* component_name_text = nullptr;
            ASSERT_EQ(component_name->GetUtf8(&component_name_text), DAS_S_OK);
            ASSERT_NE(component_name_text, nullptr);
            EXPECT_EQ(
                std::string_view{component_name_text},
                expected_component_name);
        }

        {
            DasReadOnlyString                      method_name{"echo"};
            DAS::ExportInterface::DasVariantVector dispatch_result;
            DAS::ExportInterface::DasVariantVector params;
            ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
            DasReadOnlyString input_text{"ipc-python-e2e"};
            ASSERT_EQ(params.Get()->PushBackString(input_text.Get()), DAS_S_OK);

            ASSERT_EQ(
                component->Dispatch(
                    method_name.Get(),
                    params.Get(),
                    dispatch_result.Put()),
                DAS_S_OK);
            ASSERT_NE(dispatch_result.Get(), nullptr);
            ASSERT_EQ(dispatch_result->GetSize(), 1u);

            DAS::DasPtr<IDasReadOnlyString> echo_value;
            ASSERT_EQ(
                dispatch_result->GetString(0, echo_value.Put()),
                DAS_S_OK);
            const char* echo_text = nullptr;
            ASSERT_EQ(echo_value->GetUtf8(&echo_text), DAS_S_OK);
            ASSERT_NE(echo_text, nullptr);
            EXPECT_STREQ(echo_text, "[Python] echo: ipc-python-e2e");
        }

        {
            DasReadOnlyString                      method_name{"compute"};
            DAS::ExportInterface::DasVariantVector dispatch_result;
            DAS::ExportInterface::DasVariantVector params;
            ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
            DasReadOnlyString operation{"mul"};
            ASSERT_EQ(params.Get()->PushBackString(operation.Get()), DAS_S_OK);
            ASSERT_EQ(params.Get()->PushBackInt(6), DAS_S_OK);
            ASSERT_EQ(params.Get()->PushBackInt(7), DAS_S_OK);

            ASSERT_EQ(
                component->Dispatch(
                    method_name.Get(),
                    params.Get(),
                    dispatch_result.Put()),
                DAS_S_OK);
            ASSERT_NE(dispatch_result.Get(), nullptr);
            ASSERT_EQ(dispatch_result->GetSize(), 1u);

            int64_t computed = 0;
            ASSERT_EQ(dispatch_result->GetInt(0, &computed), DAS_S_OK);
            EXPECT_EQ(computed, 42);
        }

        {
            DasReadOnlyString method_name{"failDeterministically"};
            DAS::ExportInterface::DasVariantVector dispatch_result;
            DAS::ExportInterface::DasVariantVector params;
            ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

            const DasResult result = component->Dispatch(
                method_name.Get(),
                params.Get(),
                dispatch_result.Put());
            EXPECT_NE(result, DAS_S_OK)
                << "Python deterministic non-success dispatch should not "
                   "return DAS_S_OK";
        }
    }

    void AssertPythonPackageUnloadabilityEquivalent(
        DAS::DasPtr<IDasComponent>&        component,
        DAS::DasPtr<IDasComponentFactory>& factory,
        DAS::DasPtr<IDasBase>&             factory_base,
        DAS::DasPtr<IDasPluginPackage>&    plugin_package,
        DAS::DasPtr<IDasBase>&             raw_proxy,
        DAS::Core::IPC::IHostLauncher*     launcher)
    {
        component.Reset();
        factory.Reset();
        factory_base.Reset();

        bool can_unload = false;
        ASSERT_NE(plugin_package.Get(), nullptr);
        ASSERT_EQ(plugin_package->CanUnloadNow(&can_unload), DAS_S_OK)
            << "Python package CanUnloadNow call failed after releasing "
               "component and factory proxies";
        EXPECT_TRUE(can_unload)
            << "Python package should report unloadable after component and "
               "factory proxies are released";

        plugin_package.Reset();
        raw_proxy.Reset();

        ASSERT_NE(launcher, nullptr);
        EXPECT_TRUE(launcher->IsRunning())
            << "Python host should remain alive before controlled stop";
        launcher->Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_FALSE(launcher->IsRunning())
            << "Python host should stop cleanly after proxy release";
    }

    void AssertPythonHostIpcPluginLoadAndDispatch(
        DAS::Core::IPC::MainProcess::IIpcContext&  context,
        DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher,
        const std::string&                         host_exe_path,
        const std::string&                         plugin_json_path,
        std::string_view                           expected_component_name)
    {
        ASSERT_NE(launcher.Get(), nullptr);
        ScopedHostStop guard{launcher};

        uint16_t        session_id = 0;
        const DasResult start_result = launcher->Start(
            host_exe_path,
            session_id,
            IpcTestConfig::GetHostStartTimeoutMs());
        ASSERT_EQ(start_result, DAS_S_OK);
        ASSERT_TRUE(launcher->IsRunning());
        ASSERT_GT(session_id, static_cast<uint16_t>(0));

        auto raw_proxy = LoadPluginPackageForHost(
            context,
            launcher.Get(),
            plugin_json_path,
            3);
        ASSERT_NE(raw_proxy.Get(), nullptr)
            << "Python plugin must load through IIpcContext::LoadPluginAsync";

        DAS::DasPtr<IDasPluginPackage> plugin_package;
        ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK)
            << "Python LOAD_PLUGIN proxy must QI to IDasPluginPackage";

        DAS::PluginInterface::DasPluginFeature feature{};
        ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);
        EXPECT_EQ(
            feature,
            DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);

        DAS::DasPtr<IDasBase> factory_base;
        ASSERT_EQ(
            plugin_package->CreateFeatureInterface(0, factory_base.Put()),
            DAS_S_OK);
        ASSERT_NE(factory_base.Get(), nullptr);

        DAS::DasPtr<IDasComponentFactory> factory;
        ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);
        EXPECT_EQ(factory->IsSupported(DasIidOf<IDasComponent>()), DAS_S_OK);
        AssertPythonFactoryRejectsUnsupportedIid(factory.Get());

        DAS::DasPtr<IDasComponent> component;
        ASSERT_EQ(
            factory->CreateInstance(DasIidOf<IDasComponent>(), component.Put()),
            DAS_S_OK);
        ASSERT_NE(component.Get(), nullptr);

        AssertPythonComponentDispatchBehavior(
            component.Get(),
            expected_component_name);
        EXPECT_TRUE(launcher->IsRunning())
            << "Python host should remain alive after deterministic "
               "non-success dispatch";

        AssertPythonPackageUnloadabilityEquivalent(
            component,
            factory,
            factory_base,
            plugin_package,
            raw_proxy,
            launcher.Get());
    }
} // namespace

TEST_F(IpcMultiProcessTestIntegration, HostLauncherStart)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_->Start(
        host_exe_path_,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());
    EXPECT_GT(launcher_->GetPid(), 0u);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));
    EXPECT_EQ(session_id, launcher_->GetSessionId());
}

TEST_F(IpcMultiProcessTestIntegration, HeartbeatResponse_KeepsRealHostAlive)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }
    ASSERT_FALSE(IpcTestConfig::ShouldDisableHeartbeat())
        << "Heartbeat keepalive test requires heartbeat to be enabled";

    ScopedHostStop guard{launcher_};

    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(launcher_->IsRunning());
    ASSERT_GT(launcher_->GetPid(), 0u);

    std::this_thread::sleep_for(HeartbeatTimeoutWindowForTest());

    ASSERT_TRUE(launcher_->IsRunning())
        << "Real Host should stay alive when HEARTBEAT RESPONSE refreshes the "
           "connection timestamp";

    AssertQueryMainProcessStringIpcSucceeds(
        GetContext(),
        launcher_.Get(),
        "Hello from heartbeat keepalive");
}

TEST_F(
    IpcMultiProcessTestIntegration,
    HeartbeatTimeout_SuspendedRealHostIsTerminated)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }
    ASSERT_FALSE(IpcTestConfig::ShouldDisableHeartbeat())
        << "Heartbeat timeout test requires heartbeat to be enabled";

    ScopedHostStop guard{launcher_};

    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(launcher_->IsRunning());

    const uint32_t host_pid = launcher_->GetPid();
    ASSERT_GT(host_pid, 0u);

    {
        auto suspended = SuspendProcessForTest(host_pid);
        ASSERT_TRUE(suspended) << suspended.failure;
        ScopedProcessResumeForTest resume_guard{suspended};

        const bool terminated = WaitUntilForTest(
            [this]() { return !launcher_->IsRunning(); },
            HeartbeatTimeoutWindowForTest());
        ASSERT_TRUE(terminated)
            << "Suspended Host should be terminated by heartbeat timeout";
    }

    EXPECT_FALSE(launcher_->IsRunning());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    DAS::Core::IPC::IHostLauncher* raw_restart_launcher = nullptr;
    ASSERT_EQ(ctx_->CreateHostLauncher(&raw_restart_launcher), DAS_S_OK);
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> restart_launcher(
        raw_restart_launcher);
    ASSERT_NE(restart_launcher.Get(), nullptr);
    ScopedHostStop restart_guard{restart_launcher};

    uint16_t restarted_session_id = 0;
    result = restart_launcher->Start(
        host_exe_path_,
        restarted_session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(restart_launcher->IsRunning());
    ASSERT_GT(restarted_session_id, static_cast<uint16_t>(0));
    EXPECT_EQ(restarted_session_id, restart_launcher->GetSessionId());

    AssertQueryMainProcessStringIpcSucceeds(
        GetContext(),
        restart_launcher.Get(),
        "Hello from heartbeat recovery");
}

// Phase 80.2 Plan 03 Task 3：phase gate 必跑项（Warning #4 负向 ASAN 断言）
// 验证真实多进程心跳超时路径（ConnectionManager::heartbeat_thread_ ->
// :613 NotifyHeartbeatTimeout -> :614 ClearCallbacks -> :615
// TerminateIfRunning） 在 plan 01 GuardedCallback 封装 + plan 02 Shutdown 方案
// A 落地后无 heap-use-after-free（ASAN 负向断言：无 UAF 报错即 PASS）。 若
// DasHost.exe 缺失，GTEST_SKIP 范式（phase gate 多进程并发覆盖未验证， 需在具备
// DasHost.exe 的环境补跑）。
TEST_F(
    IpcMultiProcessTestIntegration,
    HeartbeatTimeout_CrossThreadDrainsInFlightCallback)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_
                     << " — phase gate 多进程并发覆盖未验证，需在具备 "
                        "DasHost.exe 的环境补跑";
    }
    ASSERT_FALSE(IpcTestConfig::ShouldDisableHeartbeat())
        << "Heartbeat cross-thread drain test requires heartbeat to be enabled";

    ScopedHostStop guard{launcher_};
    ASSERT_EQ(StartHostAndSetupRunLoop(), DAS_S_OK);
    ASSERT_TRUE(launcher_->IsRunning());

    const uint32_t host_pid = launcher_->GetPid();
    ASSERT_GT(host_pid, 0u);

    // 挂起 Host 进程使其停止响应心跳，触发真实心跳超时路径：
    // ConnectionManager::heartbeat_thread_ 检测超时 -> 阶段 2 锁外
    // NotifyHeartbeatTimeout（持 callback_mutex_ 执行 OnHeartbeatTimeout 回调）
    // -> ClearCallbacks（持同一把锁 drain）-> TerminateIfRunning。
    // 这是 plan 01 封装 + plan 02 Shutdown 方案 A 的真实多进程验证。
    {
        auto suspended = SuspendProcessForTest(host_pid);
        ASSERT_TRUE(suspended) << suspended.failure;
        ScopedProcessResumeForTest resume_guard{suspended};

        const bool terminated = WaitUntilForTest(
            [this]() { return !launcher_->IsRunning(); },
            HeartbeatTimeoutWindowForTest());
        ASSERT_TRUE(terminated)
            << "Suspended Host should be terminated by heartbeat timeout";

        // 负向 ASAN 断言：若 plan 01 封装或 plan 02 Shutdown 有 UAF，
        // ASAN 会在 NotifyHeartbeatTimeout/ClearCallbacks 并发执行时报
        // heap-use-after-free（测试进程未崩溃即 PASS，无显式断言 ——
        // ASAN 注入运行时检查）。
    }

    EXPECT_FALSE(launcher_->IsRunning());
}

TEST_F(IpcMultiProcessTestIntegration, NodeHostLauncherStart)
{
    const auto node_executable = ResolveNodeExecutableForIntegration();
    const auto node_package_root =
        NodePackageRootFromHostExePath(host_exe_path_);
    const auto node_modules_root =
        NodeModulesRootFromHostExePath(host_exe_path_);
    const auto plugin_collection_root =
        PluginCollectionRootFromHostExePath(host_exe_path_);
    const auto node_host_package =
        ValidateNodeHostPackage(node_executable, node_package_root);
    if (!node_host_package)
    {
        GTEST_SKIP() << node_host_package.message();
    }
    ASSERT_TRUE(
        AssertNoNodeRuntimeArtifactsInPluginRoot(plugin_collection_root, true));

    ScopedHostStop guard{launcher_};
    uint16_t       session_id = 0;
    const auto     result = StartNodeHostPackage(
        launcher_.Get(),
        node_executable,
        node_package_root,
        plugin_collection_root,
        node_modules_root,
        &session_id);

    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());
    EXPECT_GT(launcher_->GetPid(), 0u);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));
    EXPECT_EQ(session_id, launcher_->GetSessionId());
}

TEST_F(IpcMultiProcessTestIntegration, NodeHostLoadPluginQueryInterface)
{
    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("NodeTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "NodeTestPlugin JSON not found: " << e.what();
    }

    const auto node_executable = ResolveNodeExecutableForIntegration();
    const auto node_package_root =
        NodePackageRootFromHostExePath(host_exe_path_);
    const auto node_modules_root =
        NodeModulesRootFromHostExePath(host_exe_path_);
    const auto plugin_package_root =
        std::filesystem::path{plugin_json_path}.parent_path();
    const auto node_host_package =
        ValidateNodeHostPackage(node_executable, node_package_root);
    if (!node_host_package)
    {
        GTEST_SKIP() << node_host_package.message();
    }
    ASSERT_TRUE(AssertNoNodeRuntimeArtifactsInPluginRoot(
        plugin_package_root,
        IsPluginCollectionRoot(plugin_package_root, host_exe_path_)));

    ScopedHostStop guard{launcher_};
    uint16_t       session_id = 0;
    auto           result = StartNodeHostPackage(
        launcher_.Get(),
        node_executable,
        node_package_root,
        plugin_package_root,
        node_modules_root,
        &session_id);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(launcher_->IsRunning());
    ASSERT_GT(session_id, static_cast<uint16_t>(0));

    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::GetPluginLoadTimeout());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Node LOAD_PLUGIN wait failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    ASSERT_NE(proxy, nullptr);

    DAS::DasPtr<IDasBase> raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK)
        << "Node LOAD_PLUGIN proxy must QI to IDasPluginPackage";

    bool can_unload = false;
    EXPECT_EQ(plugin_package->CanUnloadNow(&can_unload), DAS_S_OK);
    EXPECT_TRUE(can_unload);

    DAS::PluginInterface::DasPluginFeature feature{};
    EXPECT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);
    EXPECT_EQ(
        feature,
        DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);

    DAS::PluginInterface::DasPluginFeature out_of_range_feature{};
    EXPECT_NE(plugin_package->EnumFeature(1, &out_of_range_feature), DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning())
        << "Node host should remain alive after JS non-success result";
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_LoadNodePlugin)
{
    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("NodeTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "NodeTestPlugin JSON not found: " << e.what();
    }

    const auto node_executable = ResolveNodeExecutableForIntegration();
    const auto node_package_root =
        NodePackageRootFromHostExePath(host_exe_path_);
    const auto node_modules_root =
        NodeModulesRootFromHostExePath(host_exe_path_);
    const auto plugin_package_root =
        std::filesystem::path{plugin_json_path}.parent_path();
    const auto node_host_package =
        ValidateNodeHostPackage(node_executable, node_package_root);
    if (!node_host_package)
    {
        GTEST_SKIP() << node_host_package.message();
    }
    ASSERT_TRUE(AssertNoNodeRuntimeArtifactsInPluginRoot(
        plugin_package_root,
        IsPluginCollectionRoot(plugin_package_root, host_exe_path_)));

    ScopedHostStop guard{launcher_};
    uint16_t       session_id = 0;
    auto           result = StartNodeHostPackage(
        launcher_.Get(),
        node_executable,
        node_package_root,
        plugin_package_root,
        node_modules_root,
        &session_id);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(launcher_->IsRunning());
    ASSERT_GT(session_id, static_cast<uint16_t>(0));

    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::GetPluginLoadTimeout());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Node plugin load timed out or failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    ASSERT_NE(proxy, nullptr);

    DAS::DasPtr<IDasBase> raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DAS::PluginInterface::DasPluginFeature feature{};
    ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);
    EXPECT_EQ(
        feature,
        DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);

    DAS::DasPtr<IDasBase> factory_base;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(0, factory_base.Put()),
        DAS_S_OK);
    ASSERT_NE(factory_base.Get(), nullptr);

    DAS::DasPtr<IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);
    EXPECT_EQ(factory->IsSupported(DasIidOf<IDasComponent>()), DAS_S_OK);

    DAS::DasPtr<IDasComponent> component;
    ASSERT_EQ(
        factory->CreateInstance(DasIidOf<IDasComponent>(), component.Put()),
        DAS_S_OK);
    ASSERT_NE(component.Get(), nullptr);

    AssertNodeComponentDispatchBehavior(component.Get());
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_LoadNodeFolderPlugin)
{
    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("NodeFolderTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "NodeFolderTestPlugin JSON not found: " << e.what();
    }

    const auto node_executable = ResolveNodeExecutableForIntegration();
    const auto node_package_root =
        NodePackageRootFromHostExePath(host_exe_path_);
    const auto node_modules_root =
        NodeModulesRootFromHostExePath(host_exe_path_);
    const auto plugin_package_root =
        std::filesystem::path{plugin_json_path}.parent_path();
    const auto node_host_package =
        ValidateNodeHostPackage(node_executable, node_package_root);
    if (!node_host_package)
    {
        GTEST_SKIP() << node_host_package.message();
    }
    ASSERT_FALSE(IsPluginCollectionRoot(plugin_package_root, host_exe_path_));
    ASSERT_TRUE(
        AssertNoNodeRuntimeArtifactsInPluginRoot(plugin_package_root, false));

    ScopedHostStop guard{launcher_};
    uint16_t       session_id = 0;
    auto           result = StartNodeHostPackage(
        launcher_.Get(),
        node_executable,
        node_package_root,
        plugin_package_root,
        node_modules_root,
        &session_id);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(launcher_->IsRunning());
    ASSERT_GT(session_id, static_cast<uint16_t>(0));

    auto package_proxy = LoadPluginPackageForHost(
        GetContext(),
        launcher_.Get(),
        plugin_json_path,
        3);
    ASSERT_NE(package_proxy.Get(), nullptr);

    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(package_proxy.As(plugin_package.Put()), DAS_S_OK);
    DAS::PluginInterface::DasPluginFeature feature{};
    ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);
    EXPECT_EQ(
        feature,
        DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);

    auto factory = GetComponentFactoryFromPackage(package_proxy.Get(), 0);
    ASSERT_NE(factory.Get(), nullptr);
    EXPECT_EQ(factory->IsSupported(DasIidOf<IDasComponent>()), DAS_S_OK);

    auto component = CreateComponentFromFactory(factory.Get());
    ASSERT_NE(component.Get(), nullptr);
    AssertNodeComponentDispatchBehavior(component.Get());
    EXPECT_TRUE(launcher_->IsRunning());
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_LoadPythonPlugin)
{
#ifndef DAS_EXPORT_PYTHON
    GTEST_SKIP() << "DAS_EXPORT_PYTHON is not enabled";
#endif

    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("PythonTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "PythonTestPlugin manifest not found: " << e.what();
    }

    const auto plugin_root =
        std::filesystem::path{plugin_json_path}.parent_path();
    const auto plugin_source = plugin_root / "python_test_plugin.py";
    if (!std::filesystem::is_regular_file(plugin_source))
    {
        GTEST_SKIP() << "Python plugin source not found at: "
                     << plugin_source.string();
    }

    const auto runtime_artifacts =
        ValidatePythonRuntimeArtifacts(host_exe_path_);
    if (!runtime_artifacts)
    {
        GTEST_SKIP() << runtime_artifacts.message();
    }

    const auto package_artifacts = ValidatePythonPluginPackageArtifacts(
        plugin_json_path,
        "python_test_plugin.py");
    if (!package_artifacts)
    {
        GTEST_SKIP() << package_artifacts.message();
    }

    AssertPythonHostIpcPluginLoadAndDispatch(
        GetContext(),
        launcher_,
        host_exe_path_,
        plugin_json_path,
        "PythonTestPlugin");
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_LoadPythonFolderPlugin)
{
#ifndef DAS_EXPORT_PYTHON
    GTEST_SKIP() << "DAS_EXPORT_PYTHON is not enabled";
#endif

    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    std::filesystem::path folder_manifest_path;
    ASSERT_TRUE(PreparePythonFolderTestPluginFixture(&folder_manifest_path));

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("PythonFolderTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "PythonFolderTestPlugin manifest not found: "
                     << e.what();
    }
    ASSERT_EQ(
        std::filesystem::weakly_canonical(plugin_json_path),
        std::filesystem::weakly_canonical(folder_manifest_path));

    const auto package_artifacts = ValidatePythonPluginPackageArtifacts(
        plugin_json_path,
        "python_folder_test_plugin.py");
    if (!package_artifacts)
    {
        GTEST_SKIP() << package_artifacts.message();
    }

    const auto runtime_artifacts =
        ValidatePythonRuntimeArtifacts(host_exe_path_);
    if (!runtime_artifacts)
    {
        GTEST_SKIP() << runtime_artifacts.message();
    }

    AssertPythonHostIpcPluginLoadAndDispatch(
        GetContext(),
        launcher_,
        host_exe_path_,
        plugin_json_path,
        "PythonFolderTestPlugin");
}

TEST_F(IpcMultiProcessTestIntegration, MultipleStartStop)
{
    // 测试多次启动/停止
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    for (int i = 0; i < 3; ++i)
    {
        uint16_t  session_id = 0;
        DasResult result = launcher_->Start(
            host_exe_path_,
            session_id,
            IpcTestConfig::GetHostStartTimeoutMs());
        ASSERT_EQ(result, DAS_S_OK) << "Failed at iteration " << i;
        EXPECT_TRUE(launcher_->IsRunning());
        EXPECT_GT(session_id, static_cast<uint16_t>(0));

        launcher_->Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_FALSE(launcher_->IsRunning());
    }
}

TEST_F(IpcMultiProcessTestIntegration, StopTerminatesProcess)
{
    // 测试 Stop() 正确终止进程
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    uint16_t  session_id = 0;
    DasResult result = launcher_->Start(
        host_exe_path_,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    launcher_->Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(launcher_->IsRunning());
}

TEST_F(IpcMultiProcessTestIntegration, LauncherStateBeforeStart)
{
    EXPECT_EQ(launcher_->GetSessionId(), 0u);
    EXPECT_EQ(launcher_->GetPid(), 0u);
    EXPECT_FALSE(launcher_->IsRunning());
}

// ====== 跨进程调用测试 ======

/**
 * @brief 测试跨进程加载插件
 *
 * 验证主进程通过 IPC LOAD_PLUGIN 命令加载 Host 进程的插件。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_LoadPlugin)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动 Host 进程并注册到 ConnectionManager
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    // 2. 获取插件 JSON 路径
    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 3. 通过 IIpcContext::LoadPluginAsync 加载插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load plugin: wait failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";

    // 4. 验证返回的代理对象
    ASSERT_NE(proxy, nullptr);

    DAS_CORE_LOG_INFO("Plugin loaded: proxy = {}", (void*)proxy);
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_TaskAuthoringFactory)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load plugin: wait failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    ASSERT_NE(proxy, nullptr);

    DAS::DasPtr<IDasBase> raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DasPluginFeature feature{};
    ASSERT_EQ(plugin_package->EnumFeature(2, &feature), DAS_S_OK);
    EXPECT_EQ(feature, DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);

    DAS::DasPtr<IDasBase> factory_base;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(2, factory_base.Put()),
        DAS_S_OK);
    ASSERT_NE(factory_base.Get(), nullptr);

    DAS::DasPtr<IDasTaskAuthoringSessionFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    IDasTaskAuthoringSession* session_raw = nullptr;
    ASSERT_EQ(
        factory->CreateSession(IPC_TASK_GUID, nullptr, &session_raw),
        DAS_S_OK);
    ASSERT_NE(session_raw, nullptr);
    DAS::DasPtr<IDasTaskAuthoringSession> session(session_raw);

    DAS::DasPtr<IDasJson> document;
    ASSERT_EQ(session->GetDocument(nullptr, document.Put()), DAS_S_OK);
    auto json = ToYyjson(document.Get());
    auto obj = json.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("kind")].as_string().value_or(""),
        std::string_view("formSequence"));

    DAS::DasPtr<IDasJson> apply_result;
    ASSERT_EQ(session->ApplyChange(nullptr, apply_result.Put()), DAS_S_OK);
    auto apply_json = ToYyjson(apply_result.Get());
    EXPECT_TRUE(apply_json.as_object()->contains(
        std::string_view("acceptedProperties")));

    DAS::DasPtr<IDasJson> compile_result;
    ASSERT_EQ(session->Compile(nullptr, compile_result.Put()), DAS_S_OK);
    auto compile_json = ToYyjson(compile_result.Get());
    EXPECT_TRUE(
        (*compile_json.as_object())[std::string_view("ok")].as_bool().value_or(
            false));
}

TEST_F(
    IpcMultiProcessTestIntegration,
    CrossProcess_DasMaaPiTaskAuthoringFactory)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    std::string plugin_json_path;
    try
    {
        plugin_json_path = IpcTestConfig::GetTestPluginJsonPath("DasMaaPi");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "DasMaaPi JSON not found: " << e.what();
    }

    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::GetPluginLoadTimeout());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load DasMaaPi: wait failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    ASSERT_NE(proxy, nullptr);

    DAS::DasPtr<IDasBase> raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DasPluginFeature feature{};
    ASSERT_EQ(plugin_package->EnumFeature(1, &feature), DAS_S_OK);
    EXPECT_EQ(feature, DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY);

    DAS::DasPtr<IDasBase> factory_base;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(1, factory_base.Put()),
        DAS_S_OK);
    ASSERT_NE(factory_base.Get(), nullptr);

    DAS::DasPtr<IDasTaskAuthoringSessionFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);
    DasGuid factory_guid{};
    ASSERT_EQ(factory->GetGuid(&factory_guid), DAS_S_OK);
    EXPECT_EQ(
        factory_guid,
        DAS::Core::ForeignInterfaceHost::MakeDasGuid(
            DAS_MAAPI_AUTHORING_FACTORY_GUID_TEXT));

    IDasTaskAuthoringSession* session_raw = nullptr;
    ASSERT_EQ(
        factory->CreateSession(
            DAS::Core::ForeignInterfaceHost::MakeDasGuid(
                DAS_MAAPI_TASK_GUID_TEXT),
            nullptr,
            &session_raw),
        DAS_S_OK);
    ASSERT_NE(session_raw, nullptr);
    DAS::DasPtr<IDasTaskAuthoringSession> session(session_raw);

    DAS::DasPtr<IDasJson> document;
    ASSERT_EQ(session->GetDocument(nullptr, document.Put()), DAS_S_OK);
    auto json = ToYyjson(document.Get());
    auto obj = json.as_object();
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(
        (*obj)[std::string_view("kind")].as_string().value_or(""),
        std::string_view("formSequence"));
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_TaskComponentFactory)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load plugin: wait failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    ASSERT_NE(proxy, nullptr);

    DAS::DasPtr<IDasBase> raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DasPluginFeature feature{};
    ASSERT_EQ(plugin_package->EnumFeature(3, &feature), DAS_S_OK);
    EXPECT_EQ(feature, DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY);

    DAS::DasPtr<IDasBase> factory_base;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(3, factory_base.Put()),
        DAS_S_OK);
    ASSERT_NE(factory_base.Get(), nullptr);

    DAS::DasPtr<IDasTaskComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    IDasTaskComponent* missing_component_raw = nullptr;
    ASSERT_EQ(
        factory->CreateComponent(IPC_TASK_GUID, &missing_component_raw),
        DAS_E_NOT_FOUND);
    ASSERT_EQ(missing_component_raw, nullptr);

    IDasTaskComponent* component_raw = nullptr;
    ASSERT_EQ(
        factory->CreateComponent(IPC_TASK_COMPONENT_GUID, &component_raw),
        DAS_S_OK);
    ASSERT_NE(component_raw, nullptr);
    DAS::DasPtr<IDasTaskComponent> component(component_raw);

    DAS::DasPtr<IDasJson> settings_result;
    ASSERT_EQ(
        component->ApplySettingsChange(nullptr, settings_result.Put()),
        DAS_S_OK);
    auto settings_json = ToYyjson(settings_result.Get());
    EXPECT_TRUE(settings_json.as_object()->contains(
        std::string_view("acceptedSettings")));

    DAS::DasPtr<Das::ExportInterface::IDasPortMap> do_result;
    ASSERT_EQ(component->Do(nullptr, nullptr, do_result.Put()), DAS_S_OK);
    IDasReadOnlyString* status_str = nullptr;
    DasReadOnlyString   status_key{"status"};
    ASSERT_EQ(do_result->GetString(status_key.Get(), &status_str), DAS_S_OK);
    const char* status_u8 = nullptr;
    ASSERT_EQ(status_str->GetUtf8(&status_u8), DAS_S_OK);
    EXPECT_EQ(std::string_view(status_u8 ? status_u8 : ""), "completed");
    status_str->Release();
}

/**
 * @brief 测试 Host A 通过主进程调用 Host B 的对象
 *
 * 完整的跨进程调用链：
 *   Host A → 主进程（转发）→ Host B
 *
 * HostLauncher::Start() 自动注册到 ConnectionManager。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_HostToHostCall)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 创建并启动 Host B（目标进程）
    DAS::Core::IPC::IHostLauncher* raw_host_b = nullptr;
    DasResult result = ctx_->CreateHostLauncher(&raw_host_b);
    ASSERT_EQ(result, DAS_S_OK);
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> host_b(raw_host_b);

    uint16_t session_b = 0;
    result = host_b->Start(
        host_exe_path_,
        session_b,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_b, static_cast<uint16_t>(0));

    // 2. 创建并启动 Host A（调用方进程）
    DAS::Core::IPC::IHostLauncher* raw_host_a = nullptr;
    result = ctx_->CreateHostLauncher(&raw_host_a);
    ASSERT_EQ(result, DAS_S_OK);
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> host_a(raw_host_a);

    uint16_t session_a = 0;
    result = host_a->Start(
        host_exe_path_,
        session_a,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_a, static_cast<uint16_t>(0));
    EXPECT_NE(session_a, session_b);

    // 3. 获取插件 JSON 路径
    std::string plugin1_path, plugin2_path;
    try
    {
        plugin1_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        plugin2_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception& e)
    {
        host_a->Stop();
        host_b->Stop();
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 4. 使用异步接口并发加载两个插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op_a;
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op_b;
    result =
        ctx_->LoadPluginAsync(host_a.Get(), plugin1_path.c_str(), op_a.Put());
    ASSERT_EQ(result, DAS_S_OK);
    result =
        ctx_->LoadPluginAsync(host_b.Get(), plugin2_path.c_str(), op_b.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto both = stdexec::when_all(
        DAS::Core::IPC::async_op(GetContext(), std::move(op_a)),
        DAS::Core::IPC::async_op(GetContext(), std::move(op_b)));
    auto results = DAS::Core::IPC::wait(GetContext(), std::move(both));
    ASSERT_TRUE(results.has_value()) << "when_all: wait failed";

    auto& [result_a, p_a, result_b, p_b] = *results;

    ASSERT_EQ(result_a, DAS_S_OK);
    ASSERT_EQ(result_b, DAS_S_OK);

    auto proxy_a = DAS::DasPtr<IDasBase>::Attach(p_a);
    auto proxy_b = DAS::DasPtr<IDasBase>::Attach(p_b);
    ASSERT_NE(proxy_a.Get(), proxy_b.Get());

    DAS_CORE_LOG_INFO(
        "Cross-process infrastructure verified: host_a_session = {}, "
        "host_b_session = {}",
        session_a,
        session_b);

    // 6. 清理
    host_a->Stop();
    host_b->Stop();
}

TEST_F(IpcMultiProcessTestIntegration, MixedTransport_HttpAndIpcHostToHostCall)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    ASSERT_GT(http_port_, static_cast<uint16_t>(0))
        << "SetUp did not create an HTTP-enabled MainProcess context";

    ScopedHostStop ipc_host_guard(launcher_);

    uint16_t  ipc_session = 0;
    DasResult result = launcher_->Start(
        host_exe_path_,
        ipc_session,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_GT(ipc_session, static_cast<uint16_t>(0));

    const uint16_t expected_http_session =
        static_cast<uint16_t>(ipc_session + 1);
    std::vector<std::string> http_host_args{
        "--connect-url",
        DAS_FMT_NS::format("ws://127.0.0.1:{}", http_port_),
        "--log-level",
        "warn"};
    HostProcessGuard http_host(host_exe_path_, http_host_args);
    ASSERT_TRUE(http_host.IsRunning());
    SessionOnlyHostLauncher http_launcher(
        expected_http_session,
        http_host.GetPid());

    std::string ipc_plugin_path;
    std::string http_plugin_path;
    try
    {
        ipc_plugin_path =
            IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        http_plugin_path =
            IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    auto http_package = LoadPluginPackageForHost(
        *ctx_,
        &http_launcher,
        http_plugin_path,
        /*attempts=*/100);
    ASSERT_NE(http_package.Get(), nullptr)
        << "HTTP Host plugin load failed; expected session "
        << expected_http_session;

    auto http_factory =
        GetComponentFactoryFromPackage(http_package.Get(), /*feature_index=*/0);
    ASSERT_NE(http_factory.Get(), nullptr);

    DAS::DasPtr<IDasReadOnlyString> http_remote_name;
    ASSERT_EQ(
        http_factory->GetRuntimeClassName(http_remote_name.Put()),
        DAS_S_OK);
    ASSERT_NE(http_remote_name.Get(), nullptr);

    result = ctx_->RegisterService(
        http_remote_name.Get(),
        DasIidOf<IDasReadOnlyString>());
    ASSERT_EQ(result, DAS_S_OK);

    auto ipc_package = LoadPluginPackageForHost(
        *ctx_,
        launcher_.Get(),
        ipc_plugin_path,
        /*attempts=*/1);
    ASSERT_NE(ipc_package.Get(), nullptr);

    auto ipc_factory =
        GetComponentFactoryFromPackage(ipc_package.Get(), /*feature_index=*/1);
    ASSERT_NE(ipc_factory.Get(), nullptr);

    auto ipc_component = CreateComponentFromFactory(ipc_factory.Get());
    ASSERT_NE(ipc_component.Get(), nullptr);

    DAS::ExportInterface::DasVariantVector empty_params;
    ASSERT_EQ(CreateIDasVariantVector(empty_params.Put()), DAS_S_OK);

    DasReadOnlyString method_name{"queryMainProcessString"};
    DAS::ExportInterface::DasVariantVector dispatch_result;
    result = ipc_component->Dispatch(
        method_name.Get(),
        empty_params.Get(),
        dispatch_result.Put());
    ASSERT_EQ(result, DAS_S_OK)
        << "IPC Host failed to query and call HTTP Host remote string";

    ASSERT_NE(dispatch_result.Get(), nullptr);
    ASSERT_EQ(dispatch_result->GetSize(), 1u);

    DAS::DasPtr<IDasReadOnlyString> returned_name;
    ASSERT_EQ(dispatch_result->GetString(0, returned_name.Put()), DAS_S_OK);
    ASSERT_NE(returned_name.Get(), nullptr);

    const char* returned_text = nullptr;
    ASSERT_EQ(returned_name->GetUtf8(&returned_text), DAS_S_OK);
    ASSERT_NE(returned_text, nullptr);
    EXPECT_STREQ(returned_text, "Das.ComponentFactoryImpl");

    EXPECT_EQ(
        ctx_->UnregisterService(DasIidOf<IDasReadOnlyString>()),
        DAS_S_OK);
    http_host.Stop();
    launcher_->Stop();
}
/**
 * @brief 测试加载 Java 插件
 *
 * 验证主进程通过 IPC 加载 Java 插件（JavaTestPlugin）。
 * 需要 JVM 环境可用。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_LoadJavaPlugin)
{
#ifndef DAS_EXPORT_JAVA
    GTEST_SKIP() << "DAS_EXPORT_JAVA is not enabled";
#endif
    ASSERT_TRUE(std::filesystem::exists(host_exe_path_))
        << "DasHost.exe not found at: " << host_exe_path_;

    // 获取 JavaTestPlugin JSON 路径（检查是否存在）
    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("JavaTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "JavaTestPlugin JSON not found: " << e.what();
    }

    // 检查 JAR 文件是否存在
    std::filesystem::path jar_path =
        std::filesystem::path(plugin_json_path).parent_path()
        / "JavaTestPlugin.jar";
    ASSERT_TRUE(std::filesystem::exists(jar_path))
        << "JavaTestPlugin.jar not found at: " << jar_path.string();

    // 启动 Host 进程并注册到 ConnectionManager
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    // 通过 IIpcContext::LoadPluginAsync 加载 Java 插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::
            GetPluginLoadTimeout()); // Java 插件可能需要更长时间（JVM 初始化）

    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Java plugin load timed out or failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_TRUE(DAS::IsOk(load_result)) << DAS_FMT_NS::format(
        "Failed to load Java plugin (result = {}). "
        "Ensure JVM is properly installed and JAVA_HOME is set.",
        load_result);

    // 验证返回的代理对象
    ASSERT_NE(proxy, nullptr);

    // 通过代理获取 plugin package
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    // 枚举 feature，验证 COMPONENT_FACTORY
    DAS::PluginInterface::DasPluginFeature feature;
    ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);
    EXPECT_EQ(
        feature,
        DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);

    // 通过 feature 创建 IDasComponentFactory
    DAS::DasPtr<IDasBase> factory_base;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(0, factory_base.Put()),
        DAS_S_OK);
    ASSERT_NE(factory_base.Get(), nullptr);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    // 验证 IsSupported
    DasGuid empty_guid{};
    EXPECT_EQ(factory->IsSupported(empty_guid), DAS_E_NO_IMPLEMENTATION);
    EXPECT_EQ(
        factory->IsSupported(DasIidOf<DAS::PluginInterface::IDasComponent>()),
        DAS_S_OK);

    // CreateInstance 获取 IDasComponent
    DAS::PluginInterface::IDasComponent* component_raw = nullptr;
    ASSERT_EQ(
        factory->CreateInstance(
            DasIidOf<DAS::PluginInterface::IDasComponent>(),
            &component_raw),
        DAS_S_OK);
    ASSERT_NE(component_raw, nullptr);
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component(component_raw);

    // 调用 Dispatch("echo", ...) 跨进程验证
    // 设计意图：传递空 VariantVector，Java 端应返回 DAS_E_INVALID_ARGUMENT，
    // C++ 端校验错误码传播正确
    {
        DasReadOnlyString                      method_name{"echo"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_NE(dispatch_result, DAS_S_OK)
            << "Dispatch(echo) with empty params should return error code";
    }

    // 调用 Dispatch("compute", ...) — 同理，空参数应返回错误码
    {
        DasReadOnlyString                      method_name{"compute"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_NE(dispatch_result, DAS_S_OK)
            << "Dispatch(compute) with empty params should return error code";
    }

    // 调用 Dispatch("getSessionInfo", ...)
    // getSessionInfo Java handler 不检查参数数量，直接返回 session 信息
    // 所以空参数时 dispatch_result 应为 DAS_S_OK
    {
        DasReadOnlyString                      method_name{"getSessionInfo"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_EQ(dispatch_result, DAS_S_OK)
            << "Dispatch(getSessionInfo) should succeed (handler does not "
               "validate param count)";
    }

    DAS_CORE_LOG_INFO(
        "Java plugin fully verified: load, feature enumeration, factory "
        "creation, support check, instance creation, and dispatch");
}

// ====== 主进程退出检测测试 ======

/**
 * @brief 测试 Host 进程在主进程不存在时自动退出
 *
 * 模拟场景：主进程被杀后 Host 进程能够感知并自行退出，
 * 不会变成僵尸进程。
 *
 * 方法：手动启动 DasHost.exe 并传入一个不存在的 PID 作为 --main-pid，
 * 验证 Host 进程能在合理时间内自动退出。
 */
TEST_F(
    IpcMultiProcessTestIntegration,
    ParentProcessExit_HostAutoExit_InvalidPid)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 使用一个不存在的 PID（极大值，几乎不可能有进程使用）
    constexpr uint32_t fake_main_pid = 99999999;

    // 手动启动 DasHost.exe 进程，传入不存在的 --main-pid
    boost::asio::io_context  io_ctx;
    std::vector<std::string> args;
    args.push_back("--main-pid");
    args.push_back(std::to_string(fake_main_pid));

    boost::process::v2::process host_process(
        io_ctx,
        host_exe_path_,
        args,
        boost::process::v2::process_start_dir(
            std::filesystem::path(host_exe_path_).parent_path().string()));

    uint32_t host_pid = static_cast<uint32_t>(host_process.id());
    ASSERT_GT(host_pid, 0u);

    DAS_CORE_LOG_INFO(
        "Host process started with fake main_pid = {}, host_pid = {}",
        fake_main_pid,
        host_pid);

    // 等待 Host 进程自动退出（最多等待 10 秒）
    // 父进程监控线程检测间隔为 1 秒，加上初始化时间，5 秒内应该退出
    bool host_exited = false;
    for (int i = 0; i < 100; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        boost::system::error_code ec;
        if (!host_process.running(ec))
        {
            host_exited = true;
            break;
        }
    }

    if (!host_exited)
    {
        // 如果还没退出，强制终止避免僵尸进程
        boost::system::error_code ec;
        host_process.terminate(ec);
        FAIL()
            << "Host process did not exit after 10 seconds with invalid main_pid. "
               "Parent process monitoring is not working.";
    }

    DAS_CORE_LOG_INFO("Host process exited automatically as expected");
    SUCCEED();
}

/**
 * @brief 测试 Host 进程在主进程被杀后自动退出（完整握手版本）
 *
 * 模拟场景：启动一个辅助进程作为"假主进程"，完成真实握手后杀掉它，
 * 验证 Host 能感知并退出。
 *
 * 实现方式：
 * 1. 启动 FakeMain（等待 host_pid）
 * 2. 启动 DasHost，获得 host_pid
 * 3. 通过 shared_memory 将 host_pid 传给 FakeMain
 * 4. FakeMain 创建正确命名的管道，完成握手
 * 5. 测试框架杀掉 FakeMain
 * 6. 验证 DasHost 父进程监控检测到并退出
 */
TEST_F(
    IpcMultiProcessTestIntegration,
    ParentProcessExit_HostAutoExit_KillParent)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    FakeMainProcess::KillParentSharedMemory::Cleanup(
        FakeMainProcess::KILL_PARENT_SHM_NAME);

    // 1. 获取当前可执行程序路径
    std::string test_exe_path;
    {
        char  buffer[MAX_PATH];
        DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
        {
            GTEST_SKIP() << "Failed to get current executable path";
        }
        test_exe_path = std::string(buffer, len);
    }

    // 2. 生成唯一信号名称
    std::string signal_name = FakeMainProcess::GenerateUniqueSignalName();

    // 3. 创建信号持有者（持有锁，FakeMain 会等待）
    FakeMainProcess::SignalHolder signal_holder(signal_name);

    // 4. 启动假主进程（等待 host_pid）
    //    FakeMain 会先创建共享内存，然后等待我们写入 host_pid
    boost::asio::io_context  fake_main_io_ctx;
    std::vector<std::string> fake_main_args = {
        "--fake-main",
        "--signal-name",
        signal_name};

    boost::process::v2::process fake_main(
        fake_main_io_ctx,
        test_exe_path,
        fake_main_args);

    auto fake_main_pid = static_cast<uint32_t>(fake_main.id());

    DAS_CORE_LOG_INFO("Fake main started: pid = {}", fake_main_pid);

    // 5. 启动 DasHost（连接到假主进程）
    //    先启动 DasHost 获取 host_pid，然后写入共享内存让 FakeMain 创建管道
    boost::asio::io_context  host_io_ctx;
    std::vector<std::string> host_args = {
        "--main-pid",
        std::to_string(fake_main_pid)};

    boost::process::v2::process host_process(
        host_io_ctx,
        host_exe_path_,
        host_args,
        boost::process::v2::process_start_dir(
            std::filesystem::path(host_exe_path_).parent_path().string()));

    auto host_pid = static_cast<uint32_t>(host_process.id());
    DAS_CORE_LOG_INFO(
        "Host started: pid = {}, main_pid = {}",
        host_pid,
        fake_main_pid);

    // 6. 等待 FakeMain 创建共享内存（避免竞态条件）
    bool shm_ready =
        FakeMainProcess::KillParentSharedMemory::WaitForSharedMemoryReady(
            FakeMainProcess::KILL_PARENT_SHM_NAME,
            std::chrono::seconds(10));
    ASSERT_TRUE(shm_ready) << "FakeMain did not create shared memory in time";
    DAS_CORE_LOG_INFO("Shared memory is ready");

    // 7. 将 host_pid 写入共享内存，让 FakeMain 可以创建正确命名的管道
    FakeMainProcess::KillParentSharedMemory::WriteHostPid(
        FakeMainProcess::KILL_PARENT_SHM_NAME,
        host_pid);
    DAS_CORE_LOG_INFO("Wrote host_pid = {} to shared memory", host_pid);

    // 8. 等待假主进程就绪（管道已创建）
    //    DasHost 有 1 秒的连接重试超时，应该足够让 FakeMain 创建管道
    bool ready = signal_holder.ReleaseAndWait(std::chrono::seconds(10));
    ASSERT_TRUE(ready) << "Fake main process did not become ready in time";

    // 9. 确认假主进程还在运行
    {
        boost::system::error_code ec;
        ASSERT_TRUE(fake_main.running(ec))
            << "Fake main process exited prematurely";
    }

    // 10. 等待握手完成
    bool handshake_done =
        FakeMainProcess::KillParentSharedMemory::WaitForHandshakeDone(
            FakeMainProcess::KILL_PARENT_SHM_NAME,
            std::chrono::seconds(10));
    ASSERT_TRUE(handshake_done) << "Handshake did not complete in time";
    DAS_CORE_LOG_INFO("Handshake completed");

    // 11. 确认 Host 还在运行
    {
        boost::system::error_code ec;
        ASSERT_TRUE(host_process.running(ec))
            << "Host process exited after handshake (should still be running)";
    }

    // 12. 杀掉假主进程（模拟主进程崩溃）
    {
        boost::system::error_code ec;
        fake_main.terminate(ec);
        DAS_CORE_LOG_INFO("Fake main killed: pid = {}", fake_main_pid);
    }

    // 13. 等待 Host 自动退出（父进程监控应检测到并退出）
    bool host_exited = false;
    for (int i = 0; i < 100; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        boost::system::error_code ec;
        if (!host_process.running(ec))
        {
            host_exited = true;
            break;
        }
    }

    // 清理共享内存
    FakeMainProcess::KillParentSharedMemory::Cleanup(
        FakeMainProcess::KILL_PARENT_SHM_NAME);
    FakeMainProcess::FakeMainReadySignal::Cleanup(signal_name);

    if (!host_exited)
    {
        boost::system::error_code ec;
        host_process.terminate(ec);
        FAIL() << "Host did not exit after parent was killed. "
                  "Parent process monitoring is not working.";
    }

    DAS_CORE_LOG_INFO("Host exited automatically after real handshake");
}

/**
 * @brief 测试异步加载插件
 *
 * 验证 LoadPluginAsync + wait 异步接口，
 * 逐个加载两个插件。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_AsyncLoadPlugins)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动第一个 Host 进程
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    // 2. 获取插件 JSON 路径
    std::string plugin1_path, plugin2_path;
    try
    {
        plugin1_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        plugin2_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 3. 在第一个 Host 上使用异步接口加载插件1
    DAS::DasPtr<IDasBase> proxy1;
    {
        DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
        result = ctx_->LoadPluginAsync(
            launcher_.Get(),
            plugin1_path.c_str(),
            op.Put());
        ASSERT_EQ(result, DAS_S_OK);
        auto opt = DAS::Core::IPC::wait(
            GetContext(),
            DAS::Core::IPC::async_op(GetContext(), std::move(op)));
        ASSERT_TRUE(opt.has_value()) << "Load plugin 1: wait failed";

        auto& [load_result, loaded_proxy] = *opt;
        ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin 1 failed";
        proxy1 = DAS::DasPtr<IDasBase>::Attach(loaded_proxy);
    }

    // 4. 启动第二个 Host 进程
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher2;
    {
        DAS::Core::IPC::IHostLauncher* raw_launcher = nullptr;
        result = ctx_->CreateHostLauncher(&raw_launcher);
        ASSERT_EQ(result, DAS_S_OK);
        ASSERT_NE(raw_launcher, nullptr);
        launcher2 = DAS::DasPtr<DAS::Core::IPC::IHostLauncher>(raw_launcher);

        uint16_t session_id2 = 0;
        result = launcher2->Start(
            host_exe_path_,
            session_id2,
            IpcTestConfig::GetHostStartTimeoutMs());
        ASSERT_EQ(result, DAS_S_OK);
    }

    // 5. 在第二个 Host 上使用异步接口加载插件2
    DAS::DasPtr<IDasBase> proxy2;
    {
        DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
        result = ctx_->LoadPluginAsync(
            launcher2.Get(),
            plugin2_path.c_str(),
            op.Put());
        ASSERT_EQ(result, DAS_S_OK);
        auto opt = DAS::Core::IPC::wait(
            GetContext(),
            DAS::Core::IPC::async_op(GetContext(), std::move(op)));
        ASSERT_TRUE(opt.has_value()) << "Load plugin 2: wait failed";

        auto& [load_result, loaded_proxy] = *opt;
        ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin 2 failed";
        proxy2 = DAS::DasPtr<IDasBase>::Attach(loaded_proxy);
    }

    // 6. 验证两个代理对象均非空且不同
    ASSERT_NE(proxy1.Get(), proxy2.Get());

    DAS_CORE_LOG_INFO(
        "Both plugins loaded: object1 = {}, object2 = {}",
        (void*)proxy1.Get(),
        (void*)proxy2.Get());

    launcher2.Reset();
}

/**
 * @brief 测试 when_all 并发加载多个插件
 *
 * 使用 stdexec::when_all 在不同 Host 上并发加载两个插件，
 * 展示 stdexec 组合操作的用法。
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_AsyncLoadPlugins_WhenAll)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    // 1. 启动第一个 Host 进程
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_->IsRunning());

    // 2. 启动第二个 Host 进程
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher2;
    {
        DAS::Core::IPC::IHostLauncher* raw_launcher = nullptr;
        result = ctx_->CreateHostLauncher(&raw_launcher);
        ASSERT_EQ(result, DAS_S_OK);
        ASSERT_NE(raw_launcher, nullptr);
        launcher2 = DAS::DasPtr<DAS::Core::IPC::IHostLauncher>(raw_launcher);

        uint16_t session_id2 = 0;
        result = launcher2->Start(
            host_exe_path_,
            session_id2,
            IpcTestConfig::GetHostStartTimeoutMs());
        ASSERT_EQ(result, DAS_S_OK);
    }

    // 3. 获取插件 JSON 路径
    std::string plugin1_path, plugin2_path;
    try
    {
        plugin1_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        plugin2_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 4. 创建两个异步操作，分别发到不同 Host
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op1;
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op2;
    result =
        ctx_->LoadPluginAsync(launcher_.Get(), plugin1_path.c_str(), op1.Put());
    ASSERT_EQ(result, DAS_S_OK);
    result =
        ctx_->LoadPluginAsync(launcher2.Get(), plugin2_path.c_str(), op2.Put());
    ASSERT_EQ(result, DAS_S_OK);

    // 5. when_all 并发等待
    auto both = stdexec::when_all(
        DAS::Core::IPC::async_op(GetContext(), std::move(op1)),
        DAS::Core::IPC::async_op(GetContext(), std::move(op2)));
    auto results = DAS::Core::IPC::wait(GetContext(), std::move(both));
    ASSERT_TRUE(results.has_value()) << "when_all: wait failed";

    auto& [result1, p1, result2, p2] = *results;

    ASSERT_EQ(result1, DAS_S_OK) << "Load plugin 1 failed";
    ASSERT_EQ(result2, DAS_S_OK) << "Load plugin 2 failed";

    // 6. 验证两个代理对象均非空且不同
    auto proxy1 = DAS::DasPtr<IDasBase>::Attach(p1);
    auto proxy2 = DAS::DasPtr<IDasBase>::Attach(p2);
    ASSERT_NE(proxy1.Get(), proxy2.Get());

    DAS_CORE_LOG_INFO(
        "Both plugins loaded via when_all: object1 = {}, object2 = {}",
        (void*)proxy1.Get(),
        (void*)proxy2.Get());

    launcher2.Reset();
}

// ====== Remote Proxy 测试 ======

/**
 * @brief 测试通过 CreateRemoteProxy 创建远程 proxy 并调用方法，
 * 同时测试 [out] 接口指针：CreateInstance 返回远程 IDasComponent*
 *
 * 验证：
 * 1. CreateRemoteProxy 支持 IDasComponentFactory 接口
 * 2. 通过 proxy 调用 IsSupported 方法返回正确结果
 */
TEST_F(IpcMultiProcessTestIntegration, RemoteProxy_ComponentFactory_IsSupported)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found";
    }

    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    std::string plugin_path =
        IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");

    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result =
        ctx_->LoadPluginAsync(launcher_.Get(), plugin_path.c_str(), op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value());

    auto& [load_result, factory_proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    ASSERT_NE(factory_proxy, nullptr);

    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(factory_proxy);
    ASSERT_NE(raw_proxy, nullptr);

    DAS::PluginInterface::DasPluginPackage package;
    ASSERT_EQ(raw_proxy.As(package.Put()), DAS_S_OK);

    // 枚举所有 feature 并验证值
    std::vector<std::pair<uint64_t, DAS::PluginInterface::DasPluginFeature>>
        features;
    {
        for (uint64_t i = 0; i < 100; ++i)
        {
            DAS::PluginInterface::DasPluginFeature feature;
            const DasResult r = package->EnumFeature(i, &feature);
            if (DAS::IsFailed(r))
            {
                DAS_CORE_LOG_INFO(
                    "Enumerated {} features from plugin package (stopped at "
                    "index {})",
                    features.size(),
                    i);
                break;
            }
            features.emplace_back(i, feature);
        }
    }

    ASSERT_FALSE(features.empty());
    EXPECT_EQ(
        features[0].second,
        DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);

    // 用索引 0 创建第一个 feature 的接口
    const auto factoryBase = package.CreateFeatureInterface(features[0].first);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factoryBase.As(factory.Put()), DAS_S_OK);

    // 空 GUID 不被支持，应返回 DAS_E_NO_IMPLEMENTATION
    DasGuid guid{};
    EXPECT_EQ(factory->IsSupported(guid), DAS_E_NO_IMPLEMENTATION);

    // 正确的 IID 应被支持
    EXPECT_EQ(
        factory->IsSupported(DasIidOf<DAS::PluginInterface::IDasComponent>()),
        DAS_S_OK);

    DAS_CORE_LOG_INFO("Remote component factory support verification passed");
}

// ====== QueryMainProcessInterface E2E 测试 ======

/**
 * @brief 测试 IpcTestPlugin1 通过 DasQueryMainProcessInterface
 *        获取主进程注册的 IDasReadOnlyString
 *
 * 完整 E2E 流程：
 * 1. 主进程注册一个 IDasReadOnlyString 到 DistributedObjectManager +
 *    RemoteObjectRegistry
 * 2. 启动 Host 进程，加载 IpcTestPlugin1
 * 3. 通过 IDasComponentFactory 创建 IDasComponent
 * 4. 调用 Dispatch("queryMainProcessString")
 * 5. 验证 Dispatch 返回成功
 */
TEST_F(IpcMultiProcessTestIntegration, QueryMainProcessInterface_E2E)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found";
    }

    // 1. 获取插件 JSON 路径
    std::string plugin_path;
    try
    {
        plugin_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 2. 通过 PostCallback（stdexec::schedule + then）在 io_context 线程中
    //    依次调用 DasRegisterMainProcessService 和
    //    DasQueryMainProcessInterface，验证主进程路径下 C API 的完整调用链。
    //
    //    IpcContext::Run() 已在 run_thread_ 中以 ScopedCurrentIpcContext
    //    设置了 TLS，PostCallback 的 lambda 在同一线程执行时 TLS 已就绪，
    //    无需在 lambda 内重复设置。

    const char*       test_string = "Hello from main process E2E";
    DasReadOnlyString service_string{test_string};

    // 创建 IDasVariantVector（在主线程提前构造，通过引用捕获入 lambda）
    DAS::ExportInterface::DasVariantVector variant_vec;
    {
        DasResult vv_result = CreateIDasVariantVector(variant_vec.Put());
        ASSERT_EQ(vv_result, DAS_S_OK) << "CreateIDasVariantVector failed";
        vv_result = variant_vec->PushBackInt(42);
        ASSERT_EQ(vv_result, DAS_S_OK) << "PushBackInt(42) failed";
        vv_result = variant_vec->PushBackInt(123);
        ASSERT_EQ(vv_result, DAS_S_OK) << "PushBackInt(123) failed";
    }

    // wait() 阻塞直到两步全部完成，返回最终 DasResult
    auto reg_opt = DAS::Core::IPC::wait(
        stdexec::then(
            stdexec::then(
                stdexec::schedule(GetContext()),
                [&]() noexcept -> DasResult
                {
                    // 注册 IDasReadOnlyString
                    DasResult reg_result = DasRegisterMainProcessService(
                        service_string.Get(),
                        DasIidOf<IDasReadOnlyString>());
                    if (DAS::IsFailed(reg_result))
                    {
                        DAS_CORE_LOG_ERROR(
                            "Main process string service registration failed: "
                            "result = {}",
                            reg_result);
                        return reg_result;
                    }

                    // 回查验证：DasQueryMainProcessInterface 能取回刚注册的对象
                    IDasBase* queried = nullptr;
                    DasResult query_result = DasQueryMainProcessInterface(
                        DasIidOf<IDasReadOnlyString>(),
                        &queried);
                    if (DAS::IsFailed(query_result))
                    {
                        DAS_CORE_LOG_ERROR(
                            "Main process string service query failed: "
                            "result = {}",
                            query_result);
                        return query_result;
                    }
                    if (queried)
                    {
                        queried->Release();
                    }

                    DAS_CORE_LOG_INFO(
                        "Main process string service registration and query "
                        "succeeded");
                    return DAS_S_OK;
                }),
            [&](DasResult prev_result) noexcept -> DasResult
            {
                if (DAS::IsFailed(prev_result))
                {
                    return prev_result;
                }

                // 注册 IDasVariantVector
                DasResult reg_result = DasRegisterMainProcessService(
                    variant_vec.Get(),
                    DasIidOf<DAS::ExportInterface::IDasVariantVector>());
                if (DAS::IsFailed(reg_result))
                {
                    DAS_CORE_LOG_ERROR(
                        "Main process variant vector service registration "
                        "failed: result = {}",
                        reg_result);
                    return reg_result;
                }

                // 回查验证：DasQueryMainProcessInterface 能取回刚注册的对象
                IDasBase* queried = nullptr;
                DasResult query_result = DasQueryMainProcessInterface(
                    DasIidOf<DAS::ExportInterface::IDasVariantVector>(),
                    &queried);
                if (DAS::IsFailed(query_result))
                {
                    DAS_CORE_LOG_ERROR(
                        "Main process variant vector service query failed: "
                        "result = {}",
                        query_result);
                    return query_result;
                }
                if (queried)
                {
                    queried->Release();
                }

                DAS_CORE_LOG_INFO(
                    "Main process variant vector service registration and "
                    "query succeeded");
                return DAS_S_OK;
            }));

    ASSERT_TRUE(reg_opt.has_value())
        << "PostCallback registration: wait failed";
    DasResult result = std::get<0>(*reg_opt);
    ASSERT_EQ(result, DAS_S_OK)
        << "DasRegisterMainProcessService / DasQueryMainProcessInterface "
           "failed in io_context thread";

    // 3. 启动 Host 进程
    result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    // 4. 加载 IpcTestPlugin1
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result =
        ctx_->LoadPluginAsync(launcher_.Get(), plugin_path.c_str(), op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load plugin: wait failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";
    ASSERT_NE(proxy, nullptr);

    DAS_CORE_LOG_INFO("Plugin loaded: proxy = {}", (void*)proxy);

    // 5. Use the proxy directly to get IDasPluginPackage
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    ASSERT_NE(raw_proxy, nullptr);

    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    // 6. EnumFeature: index=0 应为 INPUT_FACTORY, index=1 应为
    //    COMPONENT_FACTORY
    {
        DAS::PluginInterface::DasPluginFeature feature0;
        ASSERT_EQ(plugin_package->EnumFeature(0, &feature0), DAS_S_OK);
        EXPECT_EQ(
            feature0,
            DAS::PluginInterface::DAS_PLUGIN_FEATURE_INPUT_FACTORY);

        DAS::PluginInterface::DasPluginFeature feature1;
        ASSERT_EQ(plugin_package->EnumFeature(1, &feature1), DAS_S_OK);
        EXPECT_EQ(
            feature1,
            DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
    }

    // 7. CreateFeatureInterface(1) → IDasComponentFactory
    IDasBase* factory_base_raw = nullptr;
    result = plugin_package->CreateFeatureInterface(1, &factory_base_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    // 8. IsSupported + CreateInstance → IDasComponent
    ASSERT_EQ(
        factory->IsSupported(DasIidOf<DAS::PluginInterface::IDasComponent>()),
        DAS_S_OK);

    DAS::PluginInterface::IDasComponent* component_raw = nullptr;
    result = factory->CreateInstance(
        DasIidOf<DAS::PluginInterface::IDasComponent>(),
        &component_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(component_raw, nullptr);
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component(component_raw);

    // 9. Dispatch("queryMainProcessString") — 核心验证
    {
        DasReadOnlyString method_name{"queryMainProcessString"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        EXPECT_EQ(dispatch_result, DAS_S_OK)
            << "Dispatch(queryMainProcessString) failed — "
               "E2E query from Host to main process did not work "
               "(pre-existing: IDasReadOnlyString has no IPC proxy)";
    }

    // 10. Dispatch("queryMainProcessVariantVector") — 验证 IDL 定义的接口
    {
        DasReadOnlyString method_name{"queryMainProcessVariantVector"};
        DAS::ExportInterface::DasVariantVector vv_result;
        DAS::ExportInterface::DasVariantVector vv_params;
        ASSERT_EQ(CreateIDasVariantVector(vv_params.Put()), DAS_S_OK);

        DasResult dispatch_result = component->Dispatch(
            method_name.Get(),
            vv_params.Get(),
            vv_result.Put());
        EXPECT_EQ(dispatch_result, DAS_S_OK)
            << "Dispatch(queryMainProcessVariantVector) failed";

        // 验证插件返回的数据与主进程注册的一致
        int64_t val0 = 0;
        EXPECT_EQ(vv_result->GetInt(0, &val0), DAS_S_OK);
        EXPECT_EQ(val0, 42) << "VariantVector Int[0] should be 42";
    }

    DAS_CORE_LOG_INFO(
        "Host plugin successfully queried main process IDasVariantVector");

    DAS_CORE_LOG_INFO(
        "Host plugin successfully queried main process IDasReadOnlyString");
}

// ====== CrossProcess QueryMainProcessString E2E 测试 ======

/**
 * @brief 测试跨进程查询主进程字符串并验证返回值
 *
 * 完整 E2E 流程：
 * 1. 主进程注册一个 IDasReadOnlyString 到 DistributedObjectManager +
 *    RemoteObjectRegistry
 * 2. 启动 Host 进程，加载 IpcTestPlugin1
 * 3. 通过 IDasComponentFactory 创建 IDasComponent
 * 4. 调用 Dispatch("queryMainProcessString")
 * 5. 验证返回的 VariantVector 包含正确的字符串值
 */
TEST_F(IpcMultiProcessTestIntegration, CrossProcess_QueryMainProcessString)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found";
    }

    // 1. 获取插件 JSON 路径
    std::string plugin_path;
    try
    {
        plugin_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 2. 在主进程中注册一个 IDasReadOnlyString 服务
    DAS::DasPtr<IDasReadOnlyString> service_string;
    DasResult create_result = CreateIDasReadOnlyStringFromUtf8(
        "Hello from CrossProcess E2E",
        service_string.Put());
    ASSERT_EQ(create_result, DAS_S_OK) << "Failed to create test string";

    // 注册服务到主进程全局服务表
    DasResult result = ctx_->RegisterService(
        service_string.Get(),
        DasIidOf<IDasReadOnlyString>());
    ASSERT_EQ(result, DAS_S_OK) << "RegisterService failed";

    DAS_CORE_LOG_INFO("Registered IDasReadOnlyString service in main process");

    // 3. 启动 Host 进程
    result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    // 4. 加载 IpcTestPlugin1
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result =
        ctx_->LoadPluginAsync(launcher_.Get(), plugin_path.c_str(), op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load plugin: wait failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";
    ASSERT_NE(proxy, nullptr);

    DAS_CORE_LOG_INFO("Plugin loaded: proxy = {}", (void*)proxy);

    // 5. Use the proxy directly to get IDasPluginPackage
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    ASSERT_NE(raw_proxy, nullptr);

    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    // 6. CreateFeatureInterface(1) → IDasComponentFactory
    IDasBase* factory_base_raw = nullptr;
    result = plugin_package->CreateFeatureInterface(1, &factory_base_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    // 7. CreateInstance → IDasComponent
    ASSERT_EQ(
        factory->IsSupported(DasIidOf<DAS::PluginInterface::IDasComponent>()),
        DAS_S_OK);

    DAS::PluginInterface::IDasComponent* component_raw = nullptr;
    result = factory->CreateInstance(
        DasIidOf<DAS::PluginInterface::IDasComponent>(),
        &component_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(component_raw, nullptr);
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component(component_raw);

    // 8. Dispatch("queryMainProcessString") — 核心验证
    {
        DasReadOnlyString method_name{"queryMainProcessString"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        DasResult dispatch_hr = component->Dispatch(
            method_name.Get(),
            params.Get(),
            dispatch_result.Put());
        ASSERT_EQ(dispatch_hr, DAS_S_OK)
            << "Dispatch(queryMainProcessString) failed";

        // 验证返回的 VariantVector 包含 1 个元素
        ASSERT_NE(dispatch_result.Get(), nullptr);
        uint64_t size = dispatch_result->GetSize();
        ASSERT_EQ(size, 1u) << "Expected 1 element in result, got " << size;

        // 验证返回的字符串与注册的一致
        IDasReadOnlyString* out_string = nullptr;
        DasResult           get_hr = dispatch_result->GetString(0, &out_string);
        ASSERT_EQ(get_hr, DAS_S_OK) << "GetString(0) failed";
        ASSERT_NE(out_string, nullptr);

        const char* out_str = nullptr;
        DasResult   utf8_hr = out_string->GetUtf8(&out_str);
        ASSERT_EQ(utf8_hr, DAS_S_OK) << "GetUtf8 failed";
        ASSERT_NE(out_str, nullptr);
        EXPECT_STREQ(out_str, "Hello from CrossProcess E2E")
            << "Returned string does not match the registered value";

        out_string->Release();
    }

    DAS_CORE_LOG_INFO(
        "Host plugin queried main process IDasReadOnlyString and returned "
        "correct value via IPC");
}

// ==========================================================================
// CrossProcess_JavaDirectorLifecycleTest
// ==========================================================================

// {10de2795-5cb4-43d7-885a-a6e35a04bbe2}
DAS_DEFINE_CLASS_GUID(
    LifecycleCallbackComponent,
    0x10de2795,
    0x5cb4,
    0x43d7,
    0x88,
    0x5a,
    0xa6,
    0xe3,
    0x5a,
    0x04,
    0xbe,
    0xe2);

// PostCallback 完成信号 — 通过 IDasAsyncCallback + PostCallback
// 实现事件驱动通知 Dispatch() 收到 IPC 回调后调用 ctx_->PostCallback()，触发
// Do() → done = true 测试线程以 1ms 粒度轮询 done（与 DAS::Core::IPC::wait()
// 的内部模式一致）
class CompletionSignal : public IDasAsyncCallback
{
    DAS_UTILS_IDASBASE_AUTO_IMPL(CompletionSignal);

public:
    std::atomic<bool>& done_;

    explicit CompletionSignal(std::atomic<bool>& done) : done_(done) {}

    DasResult Do() noexcept override
    {
        done_ = true;
        return DAS_S_OK;
    }

    DasResult GetGuid(DasGuid*) { return DAS_E_NO_IMPLEMENTATION; }
    DasResult GetRuntimeClassName(IDasReadOnlyString**)
    {
        return DAS_E_NO_IMPLEMENTATION;
    }
    DasResult QueryInterface(const DasGuid& iid, void** pp) override
    {
        if (!pp)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (iid == DasIidOf<IDasAsyncCallback>())
        {
            AddRef();
            *pp = static_cast<IDasAsyncCallback*>(this);
            return DAS_S_OK;
        }
        if (iid == DasIidOf<IDasBase>())
        {
            AddRef();
            *pp = static_cast<IDasBase*>(this);
            return DAS_S_OK;
        }
        return DAS_E_NO_INTERFACE;
    }
};

enum class LifecycleCallbackStatusSource
{
    None,
    ArgumentReadback,
    MethodNameFallback,
};

class LifecycleCallbackComponent : public DAS::PluginInterface::IDasComponent
{
    DAS_UTILS_IDASBASE_AUTO_IMPL(LifecycleCallbackComponent);

public:
    std::atomic<bool>                          callback_received_{false};
    std::atomic<LifecycleCallbackStatusSource> received_status_source_{
        LifecycleCallbackStatusSource::None};
    std::string                               received_status_;
    DAS::Core::IPC::MainProcess::IIpcContext* ctx_ = nullptr;
    DAS::DasPtr<IDasAsyncCallback>            completion_signal_;
    bool                                      request_stop_on_callback_ = true;
    bool                                      force_dispatch_failure_ = false;

    void Configure(DAS::Core::IPC::MainProcess::IIpcContext& ctx)
    {
        ctx_ = &ctx;
    }

    DasResult Dispatch(
        IDasReadOnlyString* p_function_name,
        IDasVariantVector*  p_arguments,
        IDasVariantVector** pp_out_result) override
    {
        using namespace DAS;

        const char* func_ptr = nullptr;
        DasResult   hr = p_function_name->GetUtf8(&func_ptr);
        if (IsFailed(hr) || !func_ptr)
        {
            return hr;
        }

        std::string func = func_ptr;

        const std::string callback_prefix = "lifecycle_callback:";
        if (func == "lifecycle_callback" || func.rfind(callback_prefix, 0) == 0)
        {
            received_status_source_.store(LifecycleCallbackStatusSource::None);
            if (func.rfind(callback_prefix, 0) == 0)
            {
                received_status_ = func.substr(callback_prefix.size());
                received_status_source_.store(
                    LifecycleCallbackStatusSource::MethodNameFallback);
            }
            else if (p_arguments)
            {
                IDasReadOnlyString* status_ro = nullptr;
                hr = p_arguments->GetString(0, &status_ro);
                if (IsOk(hr) && status_ro)
                {
                    const char* status_ptr = nullptr;
                    status_ro->GetUtf8(&status_ptr);
                    if (status_ptr)
                    {
                        received_status_ = status_ptr;
                        received_status_source_.store(
                            LifecycleCallbackStatusSource::ArgumentReadback);
                    }
                    status_ro->Release();
                }
            }

            callback_received_ = true;

            DAS_CORE_LOG_INFO(
                "Lifecycle callback received: status = {}",
                received_status_);

            // 事件驱动通知：通过 PostCallback 触发 CompletionSignal
            // 测试线程以 1ms 粒度轮询 done（与 DAS::Core::IPC::wait() 一致）
            if (ctx_ && completion_signal_)
            {
                ctx_->PostCallback(completion_signal_.Get());
            }

            if (ctx_ && request_stop_on_callback_)
            {
                ctx_->RequestStop();
            }

            if (force_dispatch_failure_)
            {
                return DAS_E_FAIL;
            }
        }

        if (pp_out_result)
        {
            DAS::ExportInterface::DasVariantVector empty_result;
            DasResult hr = CreateIDasVariantVector(empty_result.Put());
            if (DAS::IsOk(hr) && empty_result.Get())
            {
                empty_result.Get()->AddRef();
                *pp_out_result = empty_result.Get();
            }
        }
        return DAS_S_OK;
    }

    DasResult GetGuid(DasGuid* p_out) override
    {
        if (p_out)
        {
            *p_out = DasIidOf<LifecycleCallbackComponent>();
        }
        return DAS_S_OK;
    }

    DasResult GetRuntimeClassName(IDasReadOnlyString** pp_out) override
    {
        return DAS_E_NO_IMPLEMENTATION;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp) override
    {
        if (!pp)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (iid == DasIidOf<LifecycleCallbackComponent>())
        {
            AddRef();
            *pp = static_cast<DAS::PluginInterface::IDasComponent*>(this);
            return DAS_S_OK;
        }
        if (iid == DasIidOf<DAS::PluginInterface::IDasComponent>())
        {
            AddRef();
            *pp = static_cast<DAS::PluginInterface::IDasComponent*>(this);
            return DAS_S_OK;
        }
        if (iid == DasIidOf<IDasBase>())
        {
            AddRef();
            *pp = static_cast<IDasBase*>(this);
            return DAS_S_OK;
        }
        return DAS_E_NO_INTERFACE;
    }

private:
};

void AssertPythonDirectorLifecycleBehavior(
    DAS::Core::IPC::MainProcess::IIpcContext&  context,
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher,
    const std::string&                         host_exe_path,
    const std::string&                         plugin_json_path,
    std::string_view                           expected_component_name,
    std::string_view                           marker,
    LifecycleCallbackComponent*                callback)
{
    ASSERT_NE(launcher.Get(), nullptr);
    ASSERT_NE(callback, nullptr);
    ScopedHostStop guard{launcher};

    uint16_t        session_id = 0;
    const DasResult start_result = launcher->Start(
        host_exe_path,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(start_result, DAS_S_OK);
    ASSERT_TRUE(launcher->IsRunning());
    ASSERT_GT(session_id, static_cast<uint16_t>(0));

    auto raw_proxy =
        LoadPluginPackageForHost(context, launcher.Get(), plugin_json_path, 3);
    ASSERT_NE(raw_proxy.Get(), nullptr)
        << "Python plugin must load through IIpcContext::LoadPluginAsync";

    DAS::DasPtr<IDasPluginPackage> plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK)
        << "Python LOAD_PLUGIN proxy must QI to IDasPluginPackage";

    DAS::PluginInterface::DasPluginFeature feature{};
    ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);
    EXPECT_EQ(
        feature,
        DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);

    DAS::DasPtr<IDasBase> factory_base;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(0, factory_base.Put()),
        DAS_S_OK);
    ASSERT_NE(factory_base.Get(), nullptr);

    DAS::DasPtr<IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);
    EXPECT_EQ(factory->IsSupported(DasIidOf<IDasComponent>()), DAS_S_OK);
    AssertPythonFactoryRejectsUnsupportedIid(factory.Get());

    DAS::DasPtr<IDasComponent> component;
    ASSERT_EQ(
        factory->CreateInstance(DasIidOf<IDasComponent>(), component.Put()),
        DAS_S_OK);
    ASSERT_NE(component.Get(), nullptr);

    AssertPythonComponentDispatchBehavior(
        component.Get(),
        expected_component_name);
    EXPECT_TRUE(launcher->IsRunning())
        << "Python host should remain alive after ordinary and failure "
           "dispatch before lifecycle release";

    std::atomic<bool> done{false};
    auto              signal = DAS::MakeDasPtr<CompletionSignal>(done);
    callback->completion_signal_ = signal;

    DAS::DasPtr<IDasComponent> director_component;
    {
        DasReadOnlyString method_name{"bridgeLifecycleTest"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        const std::string marker_text{marker};
        DasReadOnlyString marker_value{marker_text.c_str()};
        ASSERT_EQ(params.Get()->PushBackComponent(callback), DAS_S_OK);
        ASSERT_EQ(params.Get()->PushBackString(marker_value.Get()), DAS_S_OK);

        const DasResult dispatch_code = component->Dispatch(
            method_name.Get(),
            params.Get(),
            dispatch_result.Put());
        ASSERT_EQ(dispatch_code, DAS_S_OK)
            << "bridgeLifecycleTest returned DAS_E/non-success: result="
            << dispatch_code;
        ASSERT_NE(dispatch_result.Get(), nullptr);
        ASSERT_EQ(dispatch_result->GetSize(), 2u);

        DAS::DasPtr<IDasReadOnlyString> director_status;
        ASSERT_EQ(
            dispatch_result->GetString(0, director_status.Put()),
            DAS_S_OK);
        const char* director_status_text = nullptr;
        ASSERT_EQ(director_status->GetUtf8(&director_status_text), DAS_S_OK);
        ASSERT_NE(director_status_text, nullptr);
        const std::string expected_status =
            std::string{"director_created:"} + marker_text;
        EXPECT_STREQ(director_status_text, expected_status.c_str());

        ASSERT_EQ(
            dispatch_result->GetComponent(1, director_component.Put()),
            DAS_S_OK);
        ASSERT_NE(director_component.Get(), nullptr);
    }

    director_component.Reset();

    constexpr auto TIMEOUT = std::chrono::seconds(15);
    const auto     start_time = std::chrono::steady_clock::now();
    while (!done.load())
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= TIMEOUT)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const std::string expected_release_status =
        std::string{"bridge_released:Python:"} + std::string{marker};
    EXPECT_TRUE(callback->callback_received_)
        << expected_release_status << " release callback not received";
    EXPECT_NE(
        callback->received_status_.find(expected_release_status),
        std::string::npos)
        << "Unexpected Python lifecycle status: " << callback->received_status_;
    EXPECT_TRUE(launcher->IsRunning())
        << "Python host should remain alive after lifecycle release callback";

    AssertPythonPackageUnloadabilityEquivalent(
        component,
        factory,
        factory_base,
        plugin_package,
        raw_proxy,
        launcher.Get());
}

void RunCSharpBridgeLifecycleTest(
    DAS::Core::IPC::MainProcess::IIpcContext&  context,
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher,
    const std::string&                         host_exe_path,
    const std::string&                         plugin_json_path,
    std::string_view                           marker,
    std::string_view                           runtime_label,
    LifecycleCallbackComponent*                callback)
{
    ASSERT_NE(launcher.Get(), nullptr);
    ASSERT_NE(callback, nullptr);
    ScopedHostStop guard{launcher};

    uint16_t        session_id = 0;
    const DasResult start_result = launcher->Start(
        host_exe_path,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(start_result, DAS_S_OK);
    ASSERT_TRUE(launcher->IsRunning());
    ASSERT_GT(session_id, static_cast<uint16_t>(0));

    auto package_proxy =
        LoadPluginPackageForHost(context, launcher.Get(), plugin_json_path, 3);
    ASSERT_NE(package_proxy.Get(), nullptr)
        << runtime_label
        << " C# package must load through IIpcContext::LoadPluginAsync";

    DAS::DasPtr<IDasPluginPackage> plugin_package;
    ASSERT_EQ(package_proxy.As(plugin_package.Put()), DAS_S_OK)
        << runtime_label
        << " C# LOAD_PLUGIN proxy must QI to IDasPluginPackage";

    DAS::PluginInterface::DasPluginFeature feature{};
    ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);
    EXPECT_EQ(
        feature,
        DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);

    DAS::DasPtr<IDasBase> factory_base;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(0, factory_base.Put()),
        DAS_S_OK);
    ASSERT_NE(factory_base.Get(), nullptr);

    DAS::DasPtr<IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);
    ASSERT_EQ(factory->IsSupported(DasIidOf<IDasComponent>()), DAS_S_OK);

    DAS::DasPtr<IDasComponent> component;
    ASSERT_EQ(
        factory->CreateInstance(DasIidOf<IDasComponent>(), component.Put()),
        DAS_S_OK);
    ASSERT_NE(component.Get(), nullptr);

    std::atomic<bool> done{false};
    auto              signal = DAS::MakeDasPtr<CompletionSignal>(done);
    callback->completion_signal_ = signal;

    DAS::DasPtr<IDasComponent> director_component;
    {
        DasReadOnlyString method_name{"bridgeLifecycleTest"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        const std::string marker_text{marker};
        DasReadOnlyString marker_value{marker_text.c_str()};
        ASSERT_EQ(params.Get()->PushBackComponent(callback), DAS_S_OK);
        ASSERT_EQ(params.Get()->PushBackString(marker_value.Get()), DAS_S_OK);

        const DasResult dispatch_result_code = component->Dispatch(
            method_name.Get(),
            params.Get(),
            dispatch_result.Put());
        ASSERT_EQ(dispatch_result_code, DAS_S_OK)
            << runtime_label << " C# bridgeLifecycleTest dispatch must succeed";
        ASSERT_NE(dispatch_result.Get(), nullptr);
        ASSERT_EQ(dispatch_result->GetSize(), 2u);

        DAS::DasPtr<IDasReadOnlyString> director_status;
        ASSERT_EQ(
            dispatch_result->GetString(0, director_status.Put()),
            DAS_S_OK);
        const char* director_status_text = nullptr;
        ASSERT_EQ(director_status->GetUtf8(&director_status_text), DAS_S_OK);
        ASSERT_NE(director_status_text, nullptr);
        const std::string expected_director_status =
            std::string{"director_created:"} + marker_text;
        EXPECT_STREQ(director_status_text, expected_director_status.c_str());

        ASSERT_EQ(
            dispatch_result->GetComponent(1, director_component.Put()),
            DAS_S_OK);
        ASSERT_NE(director_component.Get(), nullptr)
            << runtime_label
            << " C# bridgeLifecycleTest must return a director component";
    }

    director_component.Reset();

    const std::string expected_release_status =
        std::string{"bridge_released:CSharp:"} + std::string{marker};
    EXPECT_TRUE(WaitUntilForTest(
        [&done]() { return done.load(); },
        std::chrono::seconds(15),
        std::chrono::milliseconds(1)))
        << runtime_label << " C# release callback timed out";

    EXPECT_TRUE(callback->callback_received_)
        << runtime_label << " C# bridge lifecycle callback was not received";
    EXPECT_NE(
        callback->received_status_.find(expected_release_status),
        std::string::npos)
        << "Unexpected C# lifecycle status: " << callback->received_status_;
    EXPECT_EQ(
        callback->received_status_source_.load(),
        LifecycleCallbackStatusSource::ArgumentReadback)
        << runtime_label
        << " C# bridge lifecycle callback status must come from "
           "p_arguments->GetString(0)";
    EXPECT_TRUE(launcher->IsRunning())
        << runtime_label
        << " C# host should remain alive after lifecycle release callback";
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_PythonDirectorLifecycleTest)
{
#ifndef DAS_EXPORT_PYTHON
    GTEST_SKIP() << "DAS_EXPORT_PYTHON is not enabled";
#endif

    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("PythonTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "PythonTestPlugin manifest not found: " << e.what();
    }

    const auto package_artifacts = ValidatePythonPluginPackageArtifacts(
        plugin_json_path,
        "python_test_plugin.py");
    if (!package_artifacts)
    {
        GTEST_SKIP() << package_artifacts.message();
    }

    const auto runtime_artifacts =
        ValidatePythonRuntimeArtifacts(host_exe_path_);
    if (!runtime_artifacts)
    {
        GTEST_SKIP() << runtime_artifacts.message();
    }

    auto callback = DAS::MakeDasPtr<LifecycleCallbackComponent>();
    callback->Configure(GetContext());
    callback->request_stop_on_callback_ = false;

    const DasResult reg_result = ctx_->RegisterService(
        callback.Get(),
        DasIidOf<DAS::PluginInterface::IDasComponent>());
    ASSERT_EQ(reg_result, DAS_S_OK);

    AssertPythonDirectorLifecycleBehavior(
        GetContext(),
        launcher_,
        host_exe_path_,
        plugin_json_path,
        "PythonTestPlugin",
        "python_bridge_marker",
        callback.Get());
}

TEST_F(
    IpcMultiProcessTestIntegration,
    CrossProcess_PythonFolderDirectorLifecycleTest)
{
#ifndef DAS_EXPORT_PYTHON
    GTEST_SKIP() << "DAS_EXPORT_PYTHON is not enabled";
#endif

    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    std::filesystem::path folder_manifest_path;
    ASSERT_TRUE(PreparePythonFolderTestPluginFixture(&folder_manifest_path));

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("PythonFolderTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "PythonFolderTestPlugin manifest not found: "
                     << e.what();
    }
    ASSERT_EQ(
        std::filesystem::weakly_canonical(plugin_json_path),
        std::filesystem::weakly_canonical(folder_manifest_path));

    const auto package_artifacts = ValidatePythonPluginPackageArtifacts(
        plugin_json_path,
        "python_folder_test_plugin.py");
    if (!package_artifacts)
    {
        GTEST_SKIP() << package_artifacts.message();
    }

    const auto runtime_artifacts =
        ValidatePythonRuntimeArtifacts(host_exe_path_);
    if (!runtime_artifacts)
    {
        GTEST_SKIP() << runtime_artifacts.message();
    }

    auto callback = DAS::MakeDasPtr<LifecycleCallbackComponent>();
    callback->Configure(GetContext());
    callback->request_stop_on_callback_ = false;

    const DasResult reg_result = ctx_->RegisterService(
        callback.Get(),
        DasIidOf<DAS::PluginInterface::IDasComponent>());
    ASSERT_EQ(reg_result, DAS_S_OK);

    AssertPythonDirectorLifecycleBehavior(
        GetContext(),
        launcher_,
        host_exe_path_,
        plugin_json_path,
        "PythonFolderTestPlugin",
        "python_folder_bridge_marker",
        callback.Get());
}

void AssertPythonEchoDispatchStillWorks(IDasComponent* component)
{
    ASSERT_NE(component, nullptr);

    DasReadOnlyString                      method_name{"echo"};
    DAS::ExportInterface::DasVariantVector dispatch_result;
    DAS::ExportInterface::DasVariantVector params;
    ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
    DasReadOnlyString input_text{"ipc-python-after-failure"};
    ASSERT_EQ(params.Get()->PushBackString(input_text.Get()), DAS_S_OK);

    ASSERT_EQ(
        component
            ->Dispatch(method_name.Get(), params.Get(), dispatch_result.Put()),
        DAS_S_OK);
    ASSERT_NE(dispatch_result.Get(), nullptr);
    ASSERT_EQ(dispatch_result->GetSize(), 1u);

    DAS::DasPtr<IDasReadOnlyString> echo_value;
    ASSERT_EQ(dispatch_result->GetString(0, echo_value.Put()), DAS_S_OK);
    const char* echo_text = nullptr;
    ASSERT_EQ(echo_value->GetUtf8(&echo_text), DAS_S_OK);
    ASSERT_NE(echo_text, nullptr);
    EXPECT_STREQ(echo_text, "[Python] echo: ipc-python-after-failure");
}

void ExpectPythonDispatchFailure(
    IDasComponent*                 component,
    std::string_view               method,
    IDasVariantVector*             params,
    std::string_view               failure_description,
    DAS::Core::IPC::IHostLauncher* launcher)
{
    ASSERT_NE(component, nullptr);
    ASSERT_NE(params, nullptr);
    ASSERT_NE(launcher, nullptr);

    const std::string                      method_text{method};
    DasReadOnlyString                      method_name{method_text.c_str()};
    DAS::ExportInterface::DasVariantVector dispatch_result;

    const DasResult result =
        component->Dispatch(method_name.Get(), params, dispatch_result.Put());
    EXPECT_NE(result, DAS_S_OK) << failure_description;
    EXPECT_TRUE(launcher->IsRunning())
        << "Python host should remain alive after " << failure_description;
    AssertPythonEchoDispatchStillWorks(component);
    EXPECT_TRUE(launcher->IsRunning())
        << "Python host should remain alive after post-failure echo";
}

void AssertPythonFailurePathBehavior(
    DAS::Core::IPC::MainProcess::IIpcContext&  context,
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher,
    const std::string&                         host_exe_path,
    const std::string&                         plugin_json_path,
    std::string_view                           expected_component_name)
{
    ASSERT_NE(launcher.Get(), nullptr);

    auto failing_callback = DAS::MakeDasPtr<LifecycleCallbackComponent>();
    failing_callback->Configure(context);
    failing_callback->request_stop_on_callback_ = false;
    failing_callback->force_dispatch_failure_ = true;

    const DasResult reg_result = context.RegisterService(
        failing_callback.Get(),
        DasIidOf<DAS::PluginInterface::IDasComponent>());
    ASSERT_EQ(reg_result, DAS_S_OK);

    ScopedHostStop  guard{launcher};
    uint16_t        session_id = 0;
    const DasResult start_result = launcher->Start(
        host_exe_path,
        session_id,
        IpcTestConfig::GetHostStartTimeoutMs());
    ASSERT_EQ(start_result, DAS_S_OK);
    ASSERT_TRUE(launcher->IsRunning());
    ASSERT_GT(session_id, static_cast<uint16_t>(0));

    auto raw_proxy =
        LoadPluginPackageForHost(context, launcher.Get(), plugin_json_path, 3);
    ASSERT_NE(raw_proxy.Get(), nullptr)
        << "Python plugin must load through IIpcContext::LoadPluginAsync";

    DAS::DasPtr<IDasPluginPackage> plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK)
        << "Python LOAD_PLUGIN proxy must QI to IDasPluginPackage";

    DAS::PluginInterface::DasPluginFeature feature{};
    ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);
    EXPECT_EQ(
        feature,
        DAS::PluginInterface::DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);

    DAS::DasPtr<IDasBase> factory_base;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(0, factory_base.Put()),
        DAS_S_OK);
    ASSERT_NE(factory_base.Get(), nullptr);

    DAS::DasPtr<IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);
    EXPECT_EQ(factory->IsSupported(DasIidOf<IDasComponent>()), DAS_S_OK);
    AssertPythonFactoryRejectsUnsupportedIid(factory.Get());

    DAS::DasPtr<IDasComponent> component;
    ASSERT_EQ(
        factory->CreateInstance(DasIidOf<IDasComponent>(), component.Put()),
        DAS_S_OK);
    ASSERT_NE(component.Get(), nullptr);

    AssertPythonComponentDispatchBehavior(
        component.Get(),
        expected_component_name);

    {
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        ExpectPythonDispatchFailure(
            component.Get(),
            "raisePythonException",
            params.Get(),
            "Python exception dispatch should map to non-success",
            launcher.Get());
    }

    {
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        ExpectPythonDispatchFailure(
            component.Get(),
            "unsupportedPythonMethod",
            params.Get(),
            "Unsupported Python dispatch should map to non-success",
            launcher.Get());
    }

    {
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
        ASSERT_EQ(
            params.Get()->PushBackComponent(failing_callback.Get()),
            DAS_S_OK);
        DasReadOnlyString marker{"callback_failure_marker"};
        ASSERT_EQ(params.Get()->PushBackString(marker.Get()), DAS_S_OK);

        ExpectPythonDispatchFailure(
            component.Get(),
            "bridgeLifecycleCallbackFailure",
            params.Get(),
            "Python lifecycle setup callback failure should map to non-success",
            launcher.Get());
        EXPECT_TRUE(failing_callback->callback_received_)
            << "Failing callback should have been invoked before Python "
               "returned non-success";
    }

    AssertPythonPackageUnloadabilityEquivalent(
        component,
        factory,
        factory_base,
        plugin_package,
        raw_proxy,
        launcher.Get());
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_PythonFailurePathTest)
{
#ifndef DAS_EXPORT_PYTHON
    GTEST_SKIP() << "DAS_EXPORT_PYTHON is not enabled";
#endif

    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("PythonTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "PythonTestPlugin manifest not found: " << e.what();
    }

    const auto package_artifacts = ValidatePythonPluginPackageArtifacts(
        plugin_json_path,
        "python_test_plugin.py");
    if (!package_artifacts)
    {
        GTEST_SKIP() << package_artifacts.message();
    }

    const auto runtime_artifacts =
        ValidatePythonRuntimeArtifacts(host_exe_path_);
    if (!runtime_artifacts)
    {
        GTEST_SKIP() << runtime_artifacts.message();
    }

    AssertPythonFailurePathBehavior(
        GetContext(),
        launcher_,
        host_exe_path_,
        plugin_json_path,
        "PythonTestPlugin");
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_PythonFolderFailurePathTest)
{
#ifndef DAS_EXPORT_PYTHON
    GTEST_SKIP() << "DAS_EXPORT_PYTHON is not enabled";
#endif

    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    std::filesystem::path folder_manifest_path;
    ASSERT_TRUE(PreparePythonFolderTestPluginFixture(&folder_manifest_path));

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("PythonFolderTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "PythonFolderTestPlugin manifest not found: "
                     << e.what();
    }
    ASSERT_EQ(
        std::filesystem::weakly_canonical(plugin_json_path),
        std::filesystem::weakly_canonical(folder_manifest_path));

    const auto package_artifacts = ValidatePythonPluginPackageArtifacts(
        plugin_json_path,
        "python_folder_test_plugin.py");
    if (!package_artifacts)
    {
        GTEST_SKIP() << package_artifacts.message();
    }

    const auto runtime_artifacts =
        ValidatePythonRuntimeArtifacts(host_exe_path_);
    if (!runtime_artifacts)
    {
        GTEST_SKIP() << runtime_artifacts.message();
    }

    AssertPythonFailurePathBehavior(
        GetContext(),
        launcher_,
        host_exe_path_,
        plugin_json_path,
        "PythonFolderTestPlugin");
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_NodeDirectorLifecycleTest)
{
    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("NodeTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "NodeTestPlugin JSON not found: " << e.what();
    }

    const auto node_executable = ResolveNodeExecutableForIntegration();
    const auto node_package_root =
        NodePackageRootFromHostExePath(host_exe_path_);
    const auto node_modules_root =
        NodeModulesRootFromHostExePath(host_exe_path_);
    const auto plugin_package_root =
        std::filesystem::path{plugin_json_path}.parent_path();
    const auto node_host_package =
        ValidateNodeHostPackage(node_executable, node_package_root);
    if (!node_host_package)
    {
        GTEST_SKIP() << node_host_package.message();
    }
    ASSERT_TRUE(AssertNoNodeRuntimeArtifactsInPluginRoot(
        plugin_package_root,
        IsPluginCollectionRoot(plugin_package_root, host_exe_path_)));

    auto callback = DAS::MakeDasPtr<LifecycleCallbackComponent>();
    callback->Configure(GetContext());
    callback->request_stop_on_callback_ = false;

    DasResult reg_result = ctx_->RegisterService(
        callback.Get(),
        DasIidOf<DAS::PluginInterface::IDasComponent>());
    ASSERT_EQ(reg_result, DAS_S_OK);

    ScopedHostStop guard{launcher_};
    uint16_t       session_id = 0;
    DasResult      result = StartNodeHostPackage(
        launcher_.Get(),
        node_executable,
        node_package_root,
        plugin_package_root,
        node_modules_root,
        &session_id);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_TRUE(launcher_->IsRunning());
    ASSERT_GT(session_id, static_cast<uint16_t>(0));

    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::GetPluginLoadTimeout());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Node plugin load timed out or failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK);
    ASSERT_NE(proxy, nullptr);

    DAS::DasPtr<IDasBase> raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DAS::DasPtr<IDasBase> factory_base;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(0, factory_base.Put()),
        DAS_S_OK);
    ASSERT_NE(factory_base.Get(), nullptr);

    DAS::DasPtr<IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    DAS::DasPtr<IDasComponent> component;
    ASSERT_EQ(
        factory->CreateInstance(DasIidOf<IDasComponent>(), component.Put()),
        DAS_S_OK);
    ASSERT_NE(component.Get(), nullptr);

    std::atomic<bool> done{false};
    auto              signal = DAS::MakeDasPtr<CompletionSignal>(done);
    callback->completion_signal_ = signal;

    DAS::DasPtr<IDasComponent> director_component;
    {
        DasReadOnlyString method_name{"bridgeLifecycleTest"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        DasReadOnlyString marker{"node_bridge_marker"};
        ASSERT_EQ(params.Get()->PushBackComponent(callback.Get()), DAS_S_OK);
        ASSERT_EQ(params.Get()->PushBackString(marker.Get()), DAS_S_OK);

        ASSERT_EQ(
            component->Dispatch(
                method_name.Get(),
                params.Get(),
                dispatch_result.Put()),
            DAS_S_OK);
        ASSERT_NE(dispatch_result.Get(), nullptr);
        ASSERT_EQ(dispatch_result->GetSize(), 2u);

        DAS::DasPtr<IDasReadOnlyString> director_status;
        ASSERT_EQ(
            dispatch_result->GetString(0, director_status.Put()),
            DAS_S_OK);
        const char* director_status_text = nullptr;
        ASSERT_EQ(director_status->GetUtf8(&director_status_text), DAS_S_OK);
        ASSERT_NE(director_status_text, nullptr);
        EXPECT_STREQ(
            director_status_text,
            "director_created:node_bridge_marker");

        ASSERT_EQ(
            dispatch_result->GetComponent(1, director_component.Put()),
            DAS_S_OK);
        ASSERT_NE(director_component.Get(), nullptr);
    }

    constexpr auto TIMEOUT = std::chrono::seconds(15);
    const auto     start_time = std::chrono::steady_clock::now();
    while (!done.load())
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= TIMEOUT)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(callback->callback_received_)
        << "Node bridge lifecycle callback was not received";
    EXPECT_NE(
        callback->received_status_.find(
            "bridge_released:Node:node_bridge_marker"),
        std::string::npos)
        << "Unexpected status: " << callback->received_status_;
    EXPECT_EQ(
        callback->received_status_source_.load(),
        LifecycleCallbackStatusSource::ArgumentReadback)
        << "Node bridge lifecycle callback status must come from "
           "p_arguments->GetString(0)";

    {
        DasReadOnlyString director_method{"getSessionInfo"};
        DAS::ExportInterface::DasVariantVector director_result;
        DAS::ExportInterface::DasVariantVector director_params;
        ASSERT_EQ(CreateIDasVariantVector(director_params.Put()), DAS_S_OK);
        ASSERT_EQ(
            director_component->Dispatch(
                director_method.Get(),
                director_params.Get(),
                director_result.Put()),
            DAS_S_OK);
        ASSERT_NE(director_result.Get(), nullptr);
        ASSERT_EQ(director_result->GetSize(), 3u);

        DAS::DasPtr<IDasReadOnlyString> director_language;
        ASSERT_EQ(
            director_result->GetString(0, director_language.Put()),
            DAS_S_OK);
        const char* director_language_text = nullptr;
        ASSERT_EQ(
            director_language->GetUtf8(&director_language_text),
            DAS_S_OK);
        ASSERT_NE(director_language_text, nullptr);
        EXPECT_STREQ(director_language_text, "Node");

        DAS::DasPtr<IDasReadOnlyString> director_marker;
        ASSERT_EQ(
            director_result->GetString(2, director_marker.Put()),
            DAS_S_OK);
        const char* director_marker_text = nullptr;
        ASSERT_EQ(director_marker->GetUtf8(&director_marker_text), DAS_S_OK);
        ASSERT_NE(director_marker_text, nullptr);
        EXPECT_STREQ(director_marker_text, "Director");
    }

    {
        DasReadOnlyString director_method{"getSessionInfoPromise"};
        DAS::ExportInterface::DasVariantVector director_result;
        DAS::ExportInterface::DasVariantVector director_params;
        ASSERT_EQ(CreateIDasVariantVector(director_params.Put()), DAS_S_OK);
        ASSERT_EQ(
            director_component->Dispatch(
                director_method.Get(),
                director_params.Get(),
                director_result.Put()),
            DAS_S_OK);
        ASSERT_NE(director_result.Get(), nullptr);
        ASSERT_EQ(director_result->GetSize(), 3u);

        DAS::DasPtr<IDasReadOnlyString> director_marker;
        ASSERT_EQ(
            director_result->GetString(2, director_marker.Put()),
            DAS_S_OK);
        const char* director_marker_text = nullptr;
        ASSERT_EQ(director_marker->GetUtf8(&director_marker_text), DAS_S_OK);
        ASSERT_NE(director_marker_text, nullptr);
        EXPECT_STREQ(director_marker_text, "DirectorPromise");
    }

    EXPECT_TRUE(launcher_->IsRunning())
        << "Node host should remain alive after lifecycle director callback";
}

TEST_F(
    IpcMultiProcessTestIntegration,
    CrossProcess_CSharpModernDirectorLifecycleTest)
{
#ifndef DAS_EXPORT_CSHARP
    GTEST_SKIP() << "DAS_EXPORT_CSHARP is not enabled";
#elif !defined(DAS_CSHARP_BUILD_MODERN)
    GTEST_SKIP()
        << "DAS_CSHARP_BUILD_MODERN is not enabled for IpcMultiProcessTest";
#endif

    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    std::string plugin_json_path;
    try
    {
        plugin_json_path = IpcTestConfig::GetCSharpTestPluginJsonPath(
            "DasCSharpTestPluginModern");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "DasCSharpTestPluginModern manifest not found: "
                     << e.what();
    }

    auto callback = DAS::MakeDasPtr<LifecycleCallbackComponent>();
    callback->Configure(GetContext());
    callback->request_stop_on_callback_ = false;

    const DasResult reg_result = ctx_->RegisterService(
        callback.Get(),
        DasIidOf<DAS::PluginInterface::IDasComponent>());
    ASSERT_EQ(reg_result, DAS_S_OK);

    RunCSharpBridgeLifecycleTest(
        GetContext(),
        launcher_,
        host_exe_path_,
        plugin_json_path,
        "csharp_modern_bridge_marker",
        "Modern",
        callback.Get());
}

TEST_F(
    IpcMultiProcessTestIntegration,
    CrossProcess_CSharpNet48DirectorLifecycleTest)
{
#ifndef DAS_EXPORT_CSHARP
    GTEST_SKIP() << "DAS_EXPORT_CSHARP is not enabled";
#elif !defined(DAS_CSHARP_BUILD_NET48)
    GTEST_SKIP()
        << "DAS_CSHARP_BUILD_NET48 is not enabled for IpcMultiProcessTest";
#endif

    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    std::string plugin_json_path;
    try
    {
        plugin_json_path = IpcTestConfig::GetCSharpTestPluginJsonPath(
            "DasCSharpTestPluginNet48");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "DasCSharpTestPluginNet48 manifest not found: "
                     << e.what();
    }

    auto callback = DAS::MakeDasPtr<LifecycleCallbackComponent>();
    callback->Configure(GetContext());
    callback->request_stop_on_callback_ = false;

    const DasResult reg_result = ctx_->RegisterService(
        callback.Get(),
        DasIidOf<DAS::PluginInterface::IDasComponent>());
    ASSERT_EQ(reg_result, DAS_S_OK);

    RunCSharpBridgeLifecycleTest(
        GetContext(),
        launcher_,
        host_exe_path_,
        plugin_json_path,
        "csharp_net48_bridge_marker",
        "Net48",
        callback.Get());
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_JavaDirectorLifecycleTest)
{
#ifndef DAS_EXPORT_JAVA
    GTEST_SKIP() << "DAS_EXPORT_JAVA is not enabled";
#endif
    ASSERT_TRUE(std::filesystem::exists(host_exe_path_))
        << "DasHost.exe not found at: " << host_exe_path_;

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("JavaTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "JavaTestPlugin JSON not found: " << e.what();
    }

    // 1. 创建 callback 组件并注册
    auto callback = DAS::MakeDasPtr<LifecycleCallbackComponent>();
    callback->Configure(GetContext());

    // 通过 RegisterService() 注册服务（内部完成 ObjectManager + Registry
    // 两步注册）
    DasResult reg_result = ctx_->RegisterService(
        callback.Get(),
        DasIidOf<DAS::PluginInterface::IDasComponent>());
    ASSERT_EQ(reg_result, DAS_S_OK);

    // 2. 启动 Host 进程
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    // 3. 加载 Java 插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::GetPluginLoadTimeout());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value());

    auto& [load_result, proxy] = *opt;
    ASSERT_TRUE(DAS::IsOk(load_result));

    // 4. 获取 IDasComponent
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DAS::PluginInterface::DasPluginFeature feature;
    ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);

    DAS::DasPtr<IDasBase> factory_base;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(0, factory_base.Put()),
        DAS_S_OK);
    ASSERT_NE(factory_base.Get(), nullptr);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component;
    ASSERT_EQ(
        factory->CreateInstance(
            DasIidOf<DAS::PluginInterface::IDasComponent>(),
            component.Put()),
        DAS_S_OK);
    ASSERT_NE(component.Get(), nullptr);

    // 5. Dispatch bridgeLifecycleTest
    // PushBackBase 传 IDasBase* → 验证 Java 侧 as() 向下转换
    {
        DasReadOnlyString method_name{"bridgeLifecycleTest"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        // 故意用 PushBackBase 而非 PushBackComponent：
        // 验证 IPC 传递 IDasBase* 后 Java 侧可通过 as() 向下转换为
        // IDasComponent。这不是为了方便，而是为了验证 IPC QI 恢复机制。
        params.PushBackBase(callback.Get());
        params.PushBackString(DasReadOnlyString{"bridge_test_marker"});

        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_EQ(dispatch_result, DAS_S_OK)
            << "Dispatch(bridgeLifecycleTest) failed";

        // 调试：检查返回值内容
        if (result.Get())
        {
            uint64_t size = result->GetSize();
            DAS_CORE_LOG_INFO(
                "Java lifecycle dispatch returned success: result size = {}",
                size);
            if (size > 0)
            {
                IDasReadOnlyString* elem0 = nullptr;
                DasResult           hr = result->GetString(0, &elem0);
                if (DAS::IsOk(hr) && elem0)
                {
                    const char* str = nullptr;
                    elem0->GetUtf8(&str);
                    DAS_CORE_LOG_INFO(
                        "Java lifecycle dispatch result[0] = {}",
                        str ? str : "(null)");
                    elem0->Release();
                }
            }
        }
        else
        {
            DAS_CORE_LOG_ERROR("Java lifecycle dispatch returned null result");
        }

        // 关键：不提取返回值中的 Director 对象。
        // result 包含 BridgeLifecycleDirector，但我们故意不处理它。
        // result 出作用域后 → VariantVector 释放 → Director proxy Release
        // → IPC Release → Host bridge Release → __das_bridge_release
        // → swigTakeOwnership → WeakGlobalRef → Java 对象变为 GC 可达
    }
    // ← result 析构，Director proxy 被释放

    // 6. 释放 Java 组件 proxy（不再需要）
    component.Reset();
    factory.Reset();
    factory_base.Reset();
    raw_proxy.Reset();

    // 7. 等待 Java GC 触发 Cleaner/finalize 回调
    //    事件驱动通知链路：
    //    Java GC → finalize → IPC Dispatch("lifecycle_callback")
    //    → LifecycleCallbackComponent.Dispatch()
    //    → ctx_->PostCallback(completion_signal_)
    //    → CompletionSignal.Do() → done = true
    //    测试线程以 1ms 粒度轮询 done（与 DAS::Core::IPC::wait() 一致）
    std::atomic<bool> done{false};
    auto              signal = DAS::MakeDasPtr<CompletionSignal>(done);
    callback->completion_signal_ = signal;

    DAS_CORE_LOG_INFO("Waiting for bridge release callback from Java side");

    constexpr auto TIMEOUT = std::chrono::seconds(15);
    auto           start_time = std::chrono::steady_clock::now();

    while (!done.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= TIMEOUT)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 8. 验证结果
    EXPECT_TRUE(callback->callback_received_)
        << "Bridge release callback was not received — Director bridge may not "
           "have been properly released, or GC did not collect the object";
    EXPECT_TRUE(
        callback->received_status_.find("bridge_released") != std::string::npos)
        << "Unexpected status: " << callback->received_status_;

    DAS_CORE_LOG_INFO("Java lifecycle verification passed");
}

TEST_F(IpcMultiProcessTestIntegration, CrossProcess_LuaDirectorLifecycleTest)
{
#ifndef DAS_EXPORT_LUA
    GTEST_SKIP() << "DAS_EXPORT_LUA is not enabled";
#endif
    ASSERT_TRUE(std::filesystem::exists(host_exe_path_))
        << "DasHost.exe not found at: " << host_exe_path_;

    std::string plugin_json_path;
    try
    {
        plugin_json_path =
            IpcTestConfig::GetTestPluginJsonPath("LuaTestPlugin");
    }
    catch (const std::exception& e)
    {
        GTEST_FAIL() << "LuaTestPlugin JSON not found: " << e.what();
    }

    // 1. 创建 callback 组件并注册
    auto callback = DAS::MakeDasPtr<LifecycleCallbackComponent>();
    callback->Configure(GetContext());

    DasResult reg_result = ctx_->RegisterService(
        callback.Get(),
        DasIidOf<DAS::PluginInterface::IDasComponent>());
    ASSERT_EQ(reg_result, DAS_S_OK);

    // 2. 启动 Host 进程
    DasResult result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    // 3. 加载 Lua 插件
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result = ctx_->LoadPluginAsync(
        launcher_.Get(),
        plugin_json_path.c_str(),
        op.Put(),
        IpcTestConfig::GetPluginLoadTimeout());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value());

    auto& [load_result, proxy] = *opt;
    ASSERT_TRUE(DAS::IsOk(load_result));

    // 4. 获取 IDasComponent
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    DAS::PluginInterface::DasPluginFeature feature;
    ASSERT_EQ(plugin_package->EnumFeature(0, &feature), DAS_S_OK);

    DAS::DasPtr<IDasBase> factory_base;
    ASSERT_EQ(
        plugin_package->CreateFeatureInterface(0, factory_base.Put()),
        DAS_S_OK);
    ASSERT_NE(factory_base.Get(), nullptr);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component;
    ASSERT_EQ(
        factory->CreateInstance(
            DasIidOf<DAS::PluginInterface::IDasComponent>(),
            component.Put()),
        DAS_S_OK);
    ASSERT_NE(component.Get(), nullptr);

    // 5. Dispatch bridgeLifecycleTest
    {
        DasReadOnlyString method_name{"bridgeLifecycleTest"};
        DAS::ExportInterface::DasVariantVector result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        params.PushBackBase(callback.Get());
        params.PushBackString(DasReadOnlyString{"bridge_test_marker"});

        DasResult dispatch_result =
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
        ASSERT_EQ(dispatch_result, DAS_S_OK)
            << "Dispatch(bridgeLifecycleTest) failed";

        if (result.Get())
        {
            uint64_t size = result->GetSize();
            DAS_CORE_LOG_INFO(
                "Lua lifecycle dispatch returned success: result size = {}",
                size);
            if (size > 0)
            {
                IDasReadOnlyString* elem0 = nullptr;
                DasResult           hr = result->GetString(0, &elem0);
                if (DAS::IsOk(hr) && elem0)
                {
                    const char* str = nullptr;
                    elem0->GetUtf8(&str);
                    DAS_CORE_LOG_INFO(
                        "Lua lifecycle dispatch result[0] = {}",
                        str ? str : "(null)");
                    elem0->Release();
                }
            }
        }
        else
        {
            DAS_CORE_LOG_ERROR("Lua lifecycle dispatch returned null result");
        }
    }
    // ← result 析构，Director proxy 被释放

    // 6. 释放 Lua 组件 proxy
    component.Reset();
    factory.Reset();
    factory_base.Reset();
    raw_proxy.Reset();

    // 7. 等待 Lua GC 触发 __gc → Release 回调
    //    事件驱动通知链路（对照 Java 版 Java GC → finalize → callback）：
    //    Lua GC → __gc 元方法 → Director Release → ref_count=0 → 析构
    //    → (LuaTestPlugin.dispatch_callback) → callback
    //    Dispatch("lifecycle_callback") → LifecycleCallbackComponent.Dispatch()
    //    → ctx_->PostCallback(completion_signal_)
    //    → CompletionSignal.Do() → done = true
    std::atomic<bool> done{false};
    auto              signal = DAS::MakeDasPtr<CompletionSignal>(done);
    callback->completion_signal_ = signal;

    DAS_CORE_LOG_INFO("Waiting for bridge release callback from Lua side");

    constexpr auto TIMEOUT = std::chrono::seconds(15);
    auto           start_time = std::chrono::steady_clock::now();

    while (!done.load())
    {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= TIMEOUT)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 8. 验证结果
    EXPECT_TRUE(callback->callback_received_)
        << "Bridge release callback was not received — Director bridge may not "
           "have been properly released, or GC did not collect the object";
    EXPECT_TRUE(
        callback->received_status_.find("bridge_released") != std::string::npos)
        << "Unexpected status: " << callback->received_status_;

    DAS_CORE_LOG_INFO("Lua lifecycle verification passed");
}

// ====== CrossProcess QueryMainProcessInterfaceByName E2E 测试 ======

/**
 * @brief 测试跨进程按名称查询主进程服务 (ByName)
 *
 * 完整 E2E 流程：
 * 1. 主进程通过 DasRegisterMainProcessServiceByName 注册 IDasReadOnlyString
 * 2. 启动 Host 进程，加载 IpcTestPlugin1
 * 3. 通过 IDasComponentFactory 创建 IDasComponent
 * 4. 调用 Dispatch("queryMainProcessStringByName", [name])
 *    → Host 插件内部调用 DasQueryMainProcessInterfaceByName(name)
 *    → LOOKUP_BY_NAME 跨进程 IPC → 主进程 RemoteObjectRegistry 查找
 * 5. 验证返回的字符串与注册的一致
 */
TEST_F(
    IpcMultiProcessTestIntegration,
    CrossProcess_QueryMainProcessStringByName)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found";
    }

    // 1. 获取插件 JSON 路径
    std::string plugin_path;
    try
    {
        plugin_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Plugin JSON not found: " << e.what();
    }

    // 2. 在主进程中通过 ByName API 注册一个 IDasReadOnlyString 服务
    DAS::DasPtr<IDasReadOnlyString> service_string;
    DasResult create_result = CreateIDasReadOnlyStringFromUtf8(
        "Hello from ByName CrossProcess E2E",
        service_string.Put());
    ASSERT_EQ(create_result, DAS_S_OK) << "Failed to create test string";

    const char* SERVICE_NAME = "test_string_service";

    // 在 io_context 线程中注册（BusinessThread TLS 已就绪）
    auto reg_opt = DAS::Core::IPC::wait(
        stdexec::then(
            stdexec::then(
                stdexec::schedule(GetContext()),
                [&]() noexcept -> DasResult
                {
                    return ctx_->RegisterServiceByName(
                        service_string.Get(),
                        DasIidOf<IDasReadOnlyString>(),
                        SERVICE_NAME);
                }),
            [](DasResult r) noexcept -> DasResult { return r; }));

    ASSERT_TRUE(reg_opt.has_value()) << "RegisterByName: wait failed";
    DasResult result = std::get<0>(*reg_opt);
    ASSERT_EQ(result, DAS_S_OK) << "RegisterServiceByName failed";

    DAS_CORE_LOG_INFO(
        "Registered IDasReadOnlyString with name '{}' in main process",
        SERVICE_NAME);

    // 3. 启动 Host 进程
    result = StartHostAndSetupRunLoop();
    ASSERT_EQ(result, DAS_S_OK);

    // 4. 加载 IpcTestPlugin1
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    result =
        ctx_->LoadPluginAsync(launcher_.Get(), plugin_path.c_str(), op.Put());
    ASSERT_EQ(result, DAS_S_OK);

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    ASSERT_TRUE(opt.has_value()) << "Load plugin: wait failed";

    auto& [load_result, proxy] = *opt;
    ASSERT_EQ(load_result, DAS_S_OK) << "Load plugin failed";
    ASSERT_NE(proxy, nullptr);

    // 5. Get IDasPluginPackage → IDasComponentFactory → IDasComponent
    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    ASSERT_NE(raw_proxy, nullptr);

    DAS::PluginInterface::DasPluginPackage plugin_package;
    ASSERT_EQ(raw_proxy.As(plugin_package.Put()), DAS_S_OK);

    IDasBase* factory_base_raw = nullptr;
    result = plugin_package->CreateFeatureInterface(1, &factory_base_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(factory_base_raw, nullptr);
    DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

    DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
    ASSERT_EQ(factory_base.As(factory.Put()), DAS_S_OK);

    DAS::PluginInterface::IDasComponent* component_raw = nullptr;
    result = factory->CreateInstance(
        DasIidOf<DAS::PluginInterface::IDasComponent>(),
        &component_raw);
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(component_raw, nullptr);
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> component(component_raw);

    // 6. Dispatch("queryMainProcessStringByName", [name]) — 核心验证
    {
        DasReadOnlyString method_name{"queryMainProcessStringByName"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        // 传递 service name 作为参数
        DasReadOnlyString name_param{SERVICE_NAME};
        params.PushBackString(name_param.Get());

        DasResult dispatch_hr = component->Dispatch(
            method_name.Get(),
            params.Get(),
            dispatch_result.Put());
        ASSERT_EQ(dispatch_hr, DAS_S_OK)
            << "Dispatch(queryMainProcessStringByName) failed — "
               "ByName cross-process query did not work";

        // 验证返回的 VariantVector 包含 1 个元素
        ASSERT_NE(dispatch_result.Get(), nullptr);
        uint64_t size = dispatch_result->GetSize();
        ASSERT_EQ(size, 1u) << "Expected 1 element in result, got " << size;

        // 验证返回的字符串与注册的一致
        IDasReadOnlyString* out_string = nullptr;
        DasResult           get_hr = dispatch_result->GetString(0, &out_string);
        ASSERT_EQ(get_hr, DAS_S_OK) << "GetString(0) failed";
        ASSERT_NE(out_string, nullptr);

        const char* out_str = nullptr;
        DasResult   utf8_hr = out_string->GetUtf8(&out_str);
        ASSERT_EQ(utf8_hr, DAS_S_OK) << "GetUtf8 failed";
        ASSERT_NE(out_str, nullptr);
        EXPECT_STREQ(out_str, "Hello from ByName CrossProcess E2E")
            << "Returned string does not match the registered value";

        out_string->Release();
    }

    // 7. 验证注销 ByName 后 Host 查询失败
    {
        auto unreg_opt = DAS::Core::IPC::wait(
            stdexec::then(
                stdexec::then(
                    stdexec::schedule(GetContext()),
                    [&]() noexcept -> DasResult
                    { return ctx_->UnregisterServiceByName(SERVICE_NAME); }),
                [](DasResult r) noexcept -> DasResult { return r; }));
        ASSERT_TRUE(unreg_opt.has_value()) << "UnregisterByName: wait failed";
        result = std::get<0>(*unreg_opt);
        ASSERT_EQ(result, DAS_S_OK) << "UnregisterServiceByName failed";
    }

    // 注销后 Host 应查询失败
    {
        DasReadOnlyString method_name{"queryMainProcessStringByName"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        DAS::ExportInterface::DasVariantVector params;
        ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);

        DasReadOnlyString name_param{SERVICE_NAME};
        params.PushBackString(name_param.Get());

        DasResult dispatch_hr = component->Dispatch(
            method_name.Get(),
            params.Get(),
            dispatch_result.Put());
        EXPECT_NE(dispatch_hr, DAS_S_OK)
            << "Dispatch(queryMainProcessStringByName) should fail after "
               "unregister — Host-side DasQueryMainProcessInterfaceByName "
               "must return error for unregistered name";
    }

    DAS_CORE_LOG_INFO(
        "Host plugin queried main process IDasReadOnlyString by name via "
        "LOOKUP_BY_NAME IPC, and unregister was verified");
}
