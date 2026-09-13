/**
 * @file ProcessLogRecorderIpcTest.cpp
 * @brief IDasProcessLogRecorder 跨进程 proxy Record 往返测试
 *
 * 验证 focus 日志在 IPC 模式下能跨进程投递（Pitfall #1 回归守卫）：
 *   1. 主进程注册 IDasProcessLogRecorder 为具名服务；
 *   2. 插件侧（IpcTestPlugin1）resolve proxy → 创建 IDasLogEntry →
 *      SetType/AddText/SetField → proxy->Record；
 *   3. proxy->Record 经 IPC marshal 到主进程，recorder 经接口 getter
 *      读字段（替代 dynamic_cast 到 IDasLogEntryImpl，跨进程 proxy cast
 *      返 nullptr 会丢日志）→ 写主进程 user.jsonl；
 *   4. 断言 Record 返 DAS_S_OK（非 DAS_E_INVALID_ARGUMENT——Pitfall #1
 *      症状），主进程 user.jsonl 含插件侧写入的文本。
 */

#include <Das.ExportInterface.IDasVariantVector.hpp>
#include <Das.PluginInterface.IDasComponent.hpp>
#include <Das.PluginInterface.IDasComponentFactory.Implements.hpp>
// 依赖序：IDasPluginPackage wrapper 链到 IDasTaskComponent wrapper，
// 其声明需要 DasJson/DasPortMap wrapper 类完整（IDasPortMap 又依赖
// DasStringVector wrapper 先行）。
#include <Das.ExportInterface.DasJson.hpp>
#include <Das.ExportInterface.IDasStringVector.hpp>

#include <Das.ExportInterface.IDasPortMap.hpp>
#include <Das.ExportInterface.IDasReadOnlyPortMap.hpp>
#include <Das.PluginInterface.IDasPluginPackage.hpp>
#include <IpcTestConfig.h>

#include <das/Core/Logger/DasLogSessionImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/DasTypes.hpp>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <das/IDasBase.h>
#include <das/_autogen/idl/abi/IDasLogRecorder.h>

#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace EP = Das::ExportInterface;
using Das::DasPtr;
using namespace Das::Core::IPC;

namespace
{
    // 从 IDasLogSession 取 user.jsonl 完整路径。
    std::filesystem::path ReadSessionUserJsonlPath(EP::IDasLogSession* session)
    {
        if (session == nullptr)
        {
            return {};
        }
        DasPtr<IDasReadOnlyString> path_str;
        if (DAS::IsFailed(session->GetUserJsonlPath(path_str.Put()))
            || !path_str)
        {
            return {};
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(path_str->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return {};
        }
        return std::filesystem::path{reinterpret_cast<const char8_t*>(utf8)};
    }

    class ProcessLogRecorderIpcE2ETest : public ::testing::Test
    {
    protected:
        std::string                      host_exe_path_;
        DasPtr<MainProcess::IIpcContext> ctx_;
        DasPtr<IHostLauncher>            launcher_;
        std::thread                      run_thread_;
        bool                             plugin_missing_ = false;

        // session + recorder 主进程侧持有（注入式 RAII）。
        DasPtr<EP::IDasLogSession>         session_;
        DasPtr<EP::IDasLogNotifier>        notifier_;
        DasPtr<EP::IDasProcessLogRecorder> recorder_;

        // 测试用独立 debug_dir：run 结束后整目录 RAII 删除（session 析构）。
        std::filesystem::path debug_directory_;

        void SetUp() override
        {
            host_exe_path_ = IpcTestConfig::GetDasHostPath();
            ctx_ = MainProcess::CreateIpcContext(true);
            ASSERT_NE(ctx_, nullptr);

            // 建立测试用 debug_dir（隔离每个用例的 session）。
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            debug_directory_ =
                std::filesystem::current_path() / "ipc-log-test-output"
                / ("ProcessLogRecorderIpc-" + std::to_string(stamp));
            std::filesystem::remove_all(debug_directory_);

            // 创建 session + recorder（注入式）。
            const auto directory_str = debug_directory_.u8string();
            DasPtr<IDasReadOnlyString> directory_param;
            ASSERT_EQ(
                CreateIDasReadOnlyStringFromUtf8(
                    reinterpret_cast<const char*>(directory_str.c_str()),
                    directory_param.Put()),
                DAS_S_OK);
            ASSERT_EQ(
                CreateLogSession(directory_param.Get(), session_.Put()),
                DAS_S_OK);
            // notifier 可能为空（早期环境无 g_logger sink），recorder 容忍。
            GetDasLogNotifier(notifier_.Put());
            ASSERT_EQ(
                CreateIDasProcessLogRecorder(
                    notifier_.Get(),
                    session_.Get(),
                    recorder_.Put()),
                DAS_S_OK);
            ASSERT_TRUE(recorder_);

            // 注册主进程 recorder 为具名服务。
            const ScopedCurrentIpcContext scope(
                static_cast<MainProcess::IpcContext*>(ctx_.Get()));
            ASSERT_EQ(
                ctx_->RegisterServiceByName(
                    recorder_.Get(),
                    DasIidOf<EP::IDasProcessLogRecorder>(),
                    "das.focus-log-recorder"),
                DAS_S_OK);

            const auto launcher_result =
                ctx_->CreateHostLauncher(launcher_.Put());
            ASSERT_EQ(launcher_result, DAS_S_OK);
            ASSERT_NE(launcher_.Get(), nullptr);

            run_thread_ = std::thread([this]() { ctx_->Run(); });
            for (int i = 0; i < 100 && !ctx_->GetIoContext().stopped(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        void TearDown() override
        {
            if (ctx_)
            {
                ctx_->UnregisterServiceByName("das.focus-log-recorder");
                ctx_->RequestStop();
            }
            if (run_thread_.joinable())
            {
                run_thread_.join();
            }
            launcher_.Reset();
            recorder_.Reset();
            session_.Reset();
            ctx_ = nullptr;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        DasResult StartHost()
        {
            uint16_t   session_id = 0;
            const auto result = launcher_->Start(
                host_exe_path_,
                session_id,
                IpcTestConfig::GetHostStartTimeoutMs());
            return result;
        }

        // 加载 IpcTestPlugin1 并创建 IDasComponent（index=1 工厂）。
        DasPtr<Das::PluginInterface::IDasComponent> LoadComponent()
        {
            std::string plugin_json_path;
            try
            {
                plugin_json_path =
                    IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
            }
            catch (const std::exception& e)
            {
                plugin_missing_ = true;
                return {};
            }

            DasPtr<IDasAsyncLoadPluginOperation> op;
            const auto load_result = ctx_->LoadPluginAsync(
                launcher_.Get(),
                plugin_json_path.c_str(),
                op.Put(),
                IpcTestConfig::GetPluginLoadTimeout());
            if (DAS::IsFailed(load_result) || !op)
            {
                return {};
            }
            auto opt = Das::Core::IPC::wait(
                Das::Core::IPC::async_op(*ctx_, std::move(op)));
            if (!opt.has_value())
            {
                return {};
            }
            auto& [load_hr, proxy] = *opt;
            if (DAS::IsFailed(load_hr) || proxy == nullptr)
            {
                return {};
            }
            const DasPtr<IDasBase> raw_proxy = DasPtr<IDasBase>::Attach(proxy);

            Das::PluginInterface::DasPluginPackage plugin_package;
            if (DAS::IsFailed(raw_proxy.As(plugin_package.Put())))
            {
                return {};
            }
            DasPtr<IDasBase> factory_base;
            if (DAS::IsFailed(plugin_package->CreateFeatureInterface(
                    1,
                    factory_base.Put())))
            {
                return {};
            }
            Das::PluginInterface::IDasComponentFactory* factory_raw = nullptr;
            if (DAS::IsFailed(factory_base.As(&factory_raw))
                || factory_raw == nullptr)
            {
                return {};
            }
            const DasPtr<Das::PluginInterface::IDasComponentFactory> factory =
                DasPtr<Das::PluginInterface::IDasComponentFactory>::Attach(
                    factory_raw);
            DasPtr<Das::PluginInterface::IDasComponent> component;
            if (DAS::IsFailed(factory->CreateInstance(
                    DasIidOf<Das::PluginInterface::IDasComponent>(),
                    component.Put())))
            {
                return {};
            }
            return component;
        }
    };
} // namespace

// 跨进程 proxy Record 往返（Pitfall #1 修复的回归守卫）：
// 插件经 DasQueryMainProcessInterfaceByName 拿 recorder proxy，构造 entry
// 并 SetType/AddText/SetField，调 proxy->Record。主进程 recorder 经接口
// getter 读字段（不再 dynamic_cast），写 user.jsonl + Signal(UserJsonl)。
// 断言：proxy->Record 返 DAS_S_OK（非 DAS_E_INVALID_ARGUMENT——Pitfall #1
// 症状），主进程 user.jsonl 含插件写入的 "proxy-text"。
TEST_F(ProcessLogRecorderIpcE2ETest, ProxyRecordRoundTripWritesUserJsonl)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found at: " << host_exe_path_;
    }

    ASSERT_EQ(StartHost(), DAS_S_OK);
    ASSERT_TRUE(launcher_->IsRunning());

    auto component = LoadComponent();
    if (plugin_missing_)
    {
        GTEST_SKIP() << "IpcTestPlugin1 JSON not found";
    }
    ASSERT_NE(component.Get(), nullptr);

    // 插件侧执行 processLogRecorderRoundTrip：resolve → entry → Record。
    const DasReadOnlyString method_name{"processLogRecorderRoundTrip"};
    EP::DasVariantVector    params;
    EP::DasVariantVector    result;
    ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
    ASSERT_EQ(
        component->Dispatch(method_name.Get(), params.Get(), result.Put()),
        DAS_S_OK);
    ASSERT_NE(result.Get(), nullptr);

    // 插件返回 Record 的 DasResult（int64）。必须为
    // DAS_S_OK——DAS_E_INVALID_ARGUMENT 是 Pitfall #1 症状（dynamic_cast 到
    // IDasLogEntryImpl 返 nullptr）。
    ASSERT_EQ(result->GetSize(), 1u);
    int64_t record_result = -1;
    ASSERT_EQ(result->GetInt(0, &record_result), DAS_S_OK);
    EXPECT_EQ(record_result, static_cast<int64_t>(DAS_S_OK))
        << "Record failed with code " << record_result
        << " (DAS_E_INVALID_ARGUMENT would indicate dynamic_cast regression)";

    // 验证主进程 user.jsonl 含插件写入的 "proxy-text"。
    const auto jsonl_path = ReadSessionUserJsonlPath(session_.Get());
    ASSERT_FALSE(jsonl_path.empty());
    ASSERT_TRUE(std::filesystem::exists(jsonl_path));

    // 等待 ipc 排空 + 文件可见（best-effort 短轮询）。
    for (int i = 0; i < 50; ++i)
    {
        if (std::filesystem::file_size(jsonl_path) > 0)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::ifstream input{jsonl_path};
    std::string   line;
    std::string   last;
    while (std::getline(input, line))
    {
        last = line;
    }
    EXPECT_NE(last.find("proxy-text"), std::string::npos)
        << "user.jsonl last line missing proxy-text: " << last;
    EXPECT_NE(last.find("structural"), std::string::npos)
        << "user.jsonl last line missing structural type: " << last;
    EXPECT_NE(last.find("source"), std::string::npos)
        << "user.jsonl last line missing source field: " << last;
}

// 降级路径：无 IPC context（in-process）时本地 recorder 写 user.jsonl
// 与跨进程路径产出一致。recorder 经接口 getter 读字段（同一通路，无
// dynamic_cast）。
TEST_F(ProcessLogRecorderIpcE2ETest, LocalRecorderWritesSameUserJsonl)
{
    // 直接调本地 recorder（不经 proxy）。
    DasPtr<EP::IDasLogEntry> entry;
    ASSERT_EQ(CreateIDasLogEntry(entry.Put()), DAS_S_OK);
    ASSERT_EQ(entry->SetType(EP::DAS_LOG_TYPE_INFO), DAS_S_OK);
    DasU8StringOnStack text_str{"local-text"};
    ASSERT_EQ(entry->AddText(&text_str), DAS_S_OK);

    EXPECT_EQ(recorder_->Record(entry.Get()), DAS_S_OK);

    const auto jsonl_path = ReadSessionUserJsonlPath(session_.Get());
    ASSERT_FALSE(jsonl_path.empty());
    ASSERT_TRUE(std::filesystem::exists(jsonl_path));

    std::ifstream input{jsonl_path};
    std::string   line;
    std::string   last;
    while (std::getline(input, line))
    {
        last = line;
    }
    EXPECT_NE(last.find("local-text"), std::string::npos);
}
