/**
 * @file IpcPerformanceTest.cpp
 * @brief IPC 性能基准测试骨架
 *
 * 提供 fixture 类 IpcPerformanceTest 和辅助函数/类，
 * 用于后续任务添加具体性能测试用例。
 *
 * 设计要点：
 * - fixture 的 SetUp 只创建 IIpcContext + 启动事件循环线程，不启动 Host
 * - 每个测试自行调用 StartSingleHost() 或 StartDualHost()
 * - SuppressLogDuringBenchmark RAII guard 用于在测量阶段临时抑制日志
 */

#include "BenchmarkStats.h"
#include "IpcTestConfig.h"

#include <Das.ExportInterface.IDasVariantVector.hpp>
#include <Das.PluginInterface.IDasComponent.hpp>
#include <Das.PluginInterface.IDasPluginPackage.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <das/Core/IPC/AsyncOperationImpl.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/HttpIpcServer.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/Utils/StdExecution.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Das::PluginInterface;
using namespace Das::ExportInterface;

// ====== RAII Guard ======

/**
 * @brief 在 benchmark 测量阶段临时抑制日志输出
 *
 * 构造时将 spdlog 日志级别设为 warn，析构时恢复原始级别。
 * 仅在每个测试的测量阶段使用，不影响其它阶段的日志输出。
 */
class SuppressLogDuringBenchmark
{
public:
    SuppressLogDuringBenchmark() : original_level_(spdlog::get_level())
    {
        spdlog::set_level(spdlog::level::warn);
    }

    ~SuppressLogDuringBenchmark() { spdlog::set_level(original_level_); }

    // Non-copyable, non-movable
    SuppressLogDuringBenchmark(const SuppressLogDuringBenchmark&) = delete;
    SuppressLogDuringBenchmark& operator=(const SuppressLogDuringBenchmark&) =
        delete;

private:
    spdlog::level::level_enum original_level_;
};

namespace
{
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

    class ForwardingReadOnlyString final : public IDasReadOnlyString
    {
    public:
        explicit ForwardingReadOnlyString(
            DAS::DasPtr<IDasReadOnlyString> target)
            : target_(std::move(target))
        {
        }

        DasResult DAS_STD_CALL GetUtf8(const char** out_string) override
        {
            if (!out_string)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (!target_)
            {
                return DAS_E_INVALID_POINTER;
            }

            const char* remote_text = nullptr;
            DasResult   result = target_->GetUtf8(&remote_text);
            if (DAS::IsFailed(result))
            {
                return result;
            }
            utf8_cache_ = remote_text ? remote_text : "";
            *out_string = utf8_cache_.c_str();
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL GetUtf16(
            const char16_t** out_string,
            size_t*          out_string_size) noexcept override
        {
            if (!out_string || !out_string_size)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (!target_)
            {
                return DAS_E_INVALID_POINTER;
            }

            const char16_t* remote_text = nullptr;
            size_t          remote_size = 0;
            DasResult result = target_->GetUtf16(&remote_text, &remote_size);
            if (DAS::IsFailed(result))
            {
                return result;
            }

            if (remote_size > 0 && remote_text == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }

            if (remote_size > 0)
            {
                utf16_cache_.assign(remote_text, remote_text + remote_size);
            }
            else
            {
                utf16_cache_.clear();
            }
            *out_string = utf16_cache_.data();
            *out_string_size = utf16_cache_.size();
            return DAS_S_OK;
        }

        const int32_t* CBegin() override { return nullptr; }
        const int32_t* CEnd() override { return nullptr; }

        DasBool DAS_STD_CALL Equals(IDasReadOnlyString* other) noexcept override
        {
            if (!target_)
            {
                return other == nullptr;
            }
            return target_->Equals(other);
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            if (!pp_object)
            {
                return DAS_E_INVALID_POINTER;
            }

            *pp_object = nullptr;
            if (iid == DasIidOf<IDasBase>()
                || iid == DasIidOf<IDasReadOnlyString>())
            {
                AddRef();
                *pp_object = static_cast<IDasReadOnlyString*>(this);
                return DAS_S_OK;
            }

            return DAS_E_NO_INTERFACE;
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

    private:
        DAS::DasPtr<IDasReadOnlyString> target_;
        std::string                     utf8_cache_;
        std::u16string                  utf16_cache_;
        std::atomic<uint32_t>           ref_count_{1};
    };
} // namespace

// ====== Fixture ======

/**
 * @brief IPC 性能测试夹具
 *
 * SetUp 创建 IIpcContext 并启动事件循环线程。
 * 不创建 HostLauncher，不启动 Host 进程。
 * 每个测试用例自行调用 StartSingleHost() 或 StartDualHost()。
 */
class IpcPerformanceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 0. 设置 Host 进程日志等级为 warn，抑制 info 级别日志
#ifdef DAS_WINDOWS
        _putenv_s("DAS_LOG_LEVEL", "warn");
#else
        setenv("DAS_LOG_LEVEL", "warn", 1);
#endif

        // 1. 读取 DasHost 路径
        host_exe_path_ = IpcTestConfig::GetDasHostPath();

        // 2. 创建 IPC 上下文（禁用心跳以提高性能测量准确性）
        const auto* test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        const bool use_http_context =
            test_info != nullptr
            && std::string_view(test_info->name())
                   == "MixedTransport_HttpAndIpcHostToHostPerformance";
        const bool enable_heartbeat =
            use_http_context ? false : !IpcTestConfig::ShouldDisableHeartbeat();

        if (use_http_context)
        {
            http_port_ = FindFreeLoopbackPort();
            DAS::Core::IPC::HttpIpcServer::Config http_config;
            http_config.listen_address = "127.0.0.1";
            http_config.listen_port = http_port_;

            ctx_ = std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext>(
                new DAS::Core::IPC::MainProcess::IpcContext(
                    enable_heartbeat,
                    http_config),
                DAS::Core::IPC::MainProcess::DestroyIpcContext);
        }
        else
        {
            ctx_ = DAS::Core::IPC::MainProcess::CreateIpcContextShared(
                /*enable_heartbeat=*/enable_heartbeat);
        }
        if (!ctx_)
        {
            throw std::runtime_error("Failed to create IpcContext");
        }

        // 3. 启动事件循环线程
        run_thread_ = std::thread([this]() { ctx_->Run(); });

        // 4. 等待事件循环启动
        for (int i = 0; i < 100 && !ctx_->GetIoContext().stopped(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void TearDown() override
    {
        // 1. 停止事件循环
        if (ctx_)
        {
            ctx_->RequestStop();
        }

        // 2. 等待线程结束
        if (run_thread_.joinable())
        {
            run_thread_.join();
        }

        // 3. 清理
        ctx_.reset();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    /**
     * @brief 获取 IPC Context（用于 stdexec 操作）
     */
    DAS::Core::IPC::MainProcess::IIpcContext& GetContext() { return *ctx_; }

    // ====== Host 管理辅助方法 ======

    /**
     * @brief 创建并启动一个 Host 进程
     *
     * @return DasPtr<IHostLauncher> 启动成功的 HostLauncher
     *
     * 完整链路：
     * 1. ctx_->CreateHostLauncher(&raw_launcher)
     * 2. launcher->Start(host_exe_path_, session_id, timeout)
     * 3. return DasPtr<IHostLauncher>(raw_launcher)
     */
    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> StartSingleHost()
    {
        DAS::Core::IPC::IHostLauncher* raw_launcher = nullptr;
        DasResult result = ctx_->CreateHostLauncher(&raw_launcher);
        if (DAS::IsFailed(result) || !raw_launcher)
        {
            return nullptr;
        }

        DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher(raw_launcher);

        uint16_t session_id = 0;
        result = launcher->Start(
            host_exe_path_,
            session_id,
            IpcTestConfig::GetHostStartTimeoutMs());
        if (DAS::IsFailed(result))
        {
            return nullptr;
        }

        return launcher;
    }

    /**
     * @brief 创建并启动两个 Host 进程
     *
     * @return pair<launcher1, launcher2> 两个启动成功的 HostLauncher
     *
     * 参考双 Host 启动模式（IpcMultiProcessTestIntegration.cpp:827-854）
     */
    std::pair<
        DAS::DasPtr<DAS::Core::IPC::IHostLauncher>,
        DAS::DasPtr<DAS::Core::IPC::IHostLauncher>>
    StartDualHost()
    {
        auto launcher1 = StartSingleHost();
        if (!launcher1)
        {
            return {nullptr, nullptr};
        }

        auto launcher2 = StartSingleHost();
        if (!launcher2)
        {
            return {nullptr, nullptr};
        }

        return {std::move(launcher1), std::move(launcher2)};
    }

    // ====== 插件加载辅助方法 ======

    /**
     * @brief 加载 IpcTestPlugin2 并获取 IDasComponent
     *
     * @param launcher 已启动的 HostLauncher
     * @return DasPtr<IDasComponent> 加载成功返回组件指针，失败返回 nullptr
     *
     * 完整链路：
     * 1. GetTestPluginJsonPath("IpcTestPlugin2")
     * 2. ctx_->LoadPluginAsync(launcher, plugin_path, op.Put())
     * 3. wait(async_op(ctx, op)) → 获取 proxy
     * 4. Attach proxy → DasPluginPackage
     * 5. EnumFeature(0) → 确认 COMPONENT_FACTORY
     * 6. CreateFeatureInterface(0) → IDasComponentFactory
     * 7. IsSupported(DasIidOf<IDasComponent>()) → DAS_S_OK
     * 8. CreateInstance(DasIidOf<IDasComponent>(), &component_raw) →
     * IDasComponent
     */
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> LoadTestPlugin2(
        DAS::Core::IPC::IHostLauncher* launcher)
    {
        if (!launcher)
        {
            return nullptr;
        }

        // 1. 获取插件 JSON 路径
        std::string plugin_path;
        try
        {
            plugin_path =
                IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
        }
        catch (const std::exception&)
        {
            return nullptr;
        }

        // 2. LoadPluginAsync
        DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
        DasResult                                 result =
            ctx_->LoadPluginAsync(launcher, plugin_path.c_str(), op.Put());
        if (DAS::IsFailed(result))
        {
            return nullptr;
        }

        // 3. wait(async_op) → proxy
        auto opt = DAS::Core::IPC::wait(
            GetContext(),
            DAS::Core::IPC::async_op(GetContext(), std::move(op)));
        if (!opt.has_value())
        {
            return nullptr;
        }

        auto& [load_result, proxy] = *opt;
        if (DAS::IsFailed(load_result) || !proxy)
        {
            return nullptr;
        }

        // 4. Attach proxy → DasPluginPackage
        DAS::DasPtr<IDasBase> raw_proxy;
        raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
        if (!raw_proxy)
        {
            return nullptr;
        }

        DAS::PluginInterface::DasPluginPackage plugin_package;
        if (DAS::IsFailed(raw_proxy.As(plugin_package.Put())))
        {
            return nullptr;
        }

        // 5. EnumFeature(0) → 确认 COMPONENT_FACTORY
        DAS::PluginInterface::DasPluginFeature feature;
        if (DAS::IsFailed(plugin_package->EnumFeature(0, &feature)))
        {
            return nullptr;
        }

        // 6. CreateFeatureInterface(0) → IDasComponentFactory
        IDasBase* factory_base_raw = nullptr;
        if (DAS::IsFailed(
                plugin_package->CreateFeatureInterface(0, &factory_base_raw)))
        {
            return nullptr;
        }
        DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

        DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
        if (DAS::IsFailed(factory_base.As(factory.Put())))
        {
            return nullptr;
        }

        // 7. IsSupported
        if (DAS::IsFailed(factory->IsSupported(
                DasIidOf<DAS::PluginInterface::IDasComponent>())))
        {
            return nullptr;
        }

        // 8. CreateInstance → IDasComponent
        DAS::PluginInterface::IDasComponent* component_raw = nullptr;
        if (DAS::IsFailed(factory->CreateInstance(
                DasIidOf<DAS::PluginInterface::IDasComponent>(),
                &component_raw)))
        {
            return nullptr;
        }

        return DAS::DasPtr<DAS::PluginInterface::IDasComponent>(component_raw);
    }

    /**
     * @brief 加载 IpcTestPlugin1 并获取 IDasComponent
     *
     * 与 LoadTestPlugin2 相同模式，但使用 "IpcTestPlugin1"。
     * 用于 Task 7b 反向 IPC 测试。
     *
     * @param launcher 已启动的 HostLauncher
     * @return DasPtr<IDasComponent> 加载成功返回组件指针，失败返回 nullptr
     */
    DAS::DasPtr<DAS::PluginInterface::IDasComponent> LoadTestPlugin1(
        DAS::Core::IPC::IHostLauncher* launcher)
    {
        if (!launcher)
        {
            return nullptr;
        }

        // 1. 获取插件 JSON 路径
        std::string plugin_path;
        try
        {
            plugin_path =
                IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin1");
        }
        catch (const std::exception&)
        {
            return nullptr;
        }

        // 2. LoadPluginAsync
        DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
        DasResult                                 result =
            ctx_->LoadPluginAsync(launcher, plugin_path.c_str(), op.Put());
        if (DAS::IsFailed(result))
        {
            return nullptr;
        }

        // 3. wait(async_op) → proxy
        auto opt = DAS::Core::IPC::wait(
            GetContext(),
            DAS::Core::IPC::async_op(GetContext(), std::move(op)));
        if (!opt.has_value())
        {
            return nullptr;
        }

        auto& [load_result, proxy] = *opt;
        if (DAS::IsFailed(load_result) || !proxy)
        {
            return nullptr;
        }

        // 4. Attach proxy → DasPluginPackage
        DAS::DasPtr<IDasBase> raw_proxy;
        raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
        if (!raw_proxy)
        {
            return nullptr;
        }

        DAS::PluginInterface::DasPluginPackage plugin_package;
        if (DAS::IsFailed(raw_proxy.As(plugin_package.Put())))
        {
            return nullptr;
        }

        // 5. IpcTestPlugin1 feature index 1 = COMPONENT_FACTORY
        //    (index 0 = INPUT_FACTORY)
        IDasBase* factory_base_raw = nullptr;
        if (DAS::IsFailed(
                plugin_package->CreateFeatureInterface(1, &factory_base_raw)))
        {
            return nullptr;
        }
        DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

        DAS::DasPtr<DAS::PluginInterface::IDasComponentFactory> factory;
        if (DAS::IsFailed(factory_base.As(factory.Put())))
        {
            return nullptr;
        }

        // 6. IsSupported
        if (DAS::IsFailed(factory->IsSupported(
                DasIidOf<DAS::PluginInterface::IDasComponent>())))
        {
            return nullptr;
        }

        // 7. CreateInstance → IDasComponent
        DAS::PluginInterface::IDasComponent* component_raw = nullptr;
        if (DAS::IsFailed(factory->CreateInstance(
                DasIidOf<DAS::PluginInterface::IDasComponent>(),
                &component_raw)))
        {
            return nullptr;
        }

        return DAS::DasPtr<DAS::PluginInterface::IDasComponent>(component_raw);
    }

    DAS::DasPtr<IDasBase> LoadPluginPackageForHost(
        DAS::Core::IPC::IHostLauncher* launcher,
        const std::string&             plugin_path,
        size_t                         attempts)
    {
        for (size_t attempt = 0; attempt < attempts; ++attempt)
        {
            DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
            DasResult result = ctx_->LoadPluginAsync(
                launcher,
                plugin_path.c_str(),
                op.Put(),
                IpcTestConfig::GetPluginLoadTimeout());
            if (DAS::IsOk(result))
            {
                auto opt = DAS::Core::IPC::wait(
                    GetContext(),
                    DAS::Core::IPC::async_op(GetContext(), std::move(op)));
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

        IDasBase* factory_base_raw = nullptr;
        if (DAS::IsFailed(plugin_package->CreateFeatureInterface(
                feature_index,
                &factory_base_raw)))
        {
            return {};
        }
        DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

        DAS::DasPtr<IDasComponentFactory> factory;
        if (DAS::IsFailed(factory_base.As(factory.Put())))
        {
            return {};
        }

        return factory;
    }

    // ====== 参数构造辅助函数 ======

    /**
     * @brief 创建 echo 测试参数（含一个字符串元素）
     *
     * @param str 要 echo 的字符串
     * @return DasVariantVector 包含一个字符串元素的参数向量
     */
    DAS::ExportInterface::DasVariantVector CreateStringParam(
        const std::string& str)
    {
        DAS::ExportInterface::DasVariantVector params;
        DasResult result = CreateIDasVariantVector(params.Put());
        if (DAS::IsFailed(result))
        {
            return {};
        }
        DasReadOnlyString input{str.c_str()};
        params->PushBackString(input.Get());
        return params;
    }

    /**
     * @brief 创建 compute 测试参数（含 op 字符串 + 两个整数）
     *
     * @param op 操作名称（如 "add"）
     * @param a 第一个操作数
     * @param b 第二个操作数
     * @return DasVariantVector 包含三个元素的参数向量
     */
    DAS::ExportInterface::DasVariantVector CreateComputeParams(
        const std::string& op,
        int64_t            a,
        int64_t            b)
    {
        DAS::ExportInterface::DasVariantVector params;
        DasResult result = CreateIDasVariantVector(params.Put());
        if (DAS::IsFailed(result))
        {
            return {};
        }
        DasReadOnlyString op_str{op.c_str()};
        params->PushBackString(op_str.Get());
        params->PushBackInt(a);
        params->PushBackInt(b);
        return params;
    }

    uint16_t FindFreeLoopbackPort()
    {
        boost::asio::io_context        io_context;
        boost::asio::ip::tcp::acceptor acceptor(
            io_context,
            {boost::asio::ip::make_address("127.0.0.1"), 0});
        return acceptor.local_endpoint().port();
    }

    // ====== 成员变量 ======

    std::string                                               host_exe_path_;
    std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ctx_;
    std::thread                                               run_thread_;
    uint16_t                                                  http_port_ = 0;
};

// ====== 辅助函数 ======

/**
 * @brief 打印性能测试结果的 Unicode 表格
 */
static void PrintBenchmarkResult(
    const std::string& test_name,
    size_t             iterations,
    const std::string& payload_desc,
    double             mean,
    double             p50,
    double             p95,
    double             p99,
    double             min_val,
    double             max_val,
    double             throughput)
{
    std::cout << "\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
    std::cout << "  IPC Performance: " << test_name << "\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
    std::cout << "  Iterations:     " << iterations << "\n";
    std::cout << "  Payload:        " << payload_desc << "\n";
    std::cout << "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n";
    std::cout << "  Mean Latency:   " << std::fixed << std::setprecision(2)
              << mean << " us\n";
    std::cout << "  Median (p50):   " << p50 << " us\n";
    std::cout << "  p95:            " << p95 << " us\n";
    std::cout << "  p99:            " << p99 << " us\n";
    std::cout << "  Min:            " << min_val << " us\n";
    std::cout << "  Max:            " << max_val << " us\n";
    std::cout << "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n";
    std::cout << "  Throughput:     " << std::fixed << std::setprecision(1)
              << throughput << " ops/sec\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
}

// ====== Task 3: RoundTripLatency_Compute ======

TEST_F(IpcPerformanceTest, RoundTripLatency_Compute)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found";
    }

    auto launcher = StartSingleHost();
    if (!launcher)
    {
        GTEST_SKIP() << "Failed to start Host process";
    }

    auto component = LoadTestPlugin2(launcher.Get());
    if (!component)
    {
        GTEST_SKIP() << "Failed to load IpcTestPlugin2";
    }

    constexpr size_t kWarmup = 100;
    constexpr size_t kIterations = 10000;

    // Warmup: discard first N iterations
    for (size_t i = 0; i < kWarmup; ++i)
    {
        DasReadOnlyString                      method_name{"compute"};
        DAS::ExportInterface::DasVariantVector result;
        auto params = CreateComputeParams("add", 42, 58);
        component->Dispatch(method_name.Get(), params.Get(), result.Put());
    }

    // Suppress logs during measurement
    SuppressLogDuringBenchmark log_guard;

    std::vector<double> latencies;
    latencies.reserve(kIterations);

    auto params = CreateComputeParams("add", 42, 58);

    for (size_t i = 0; i < kIterations; ++i)
    {
        DasReadOnlyString                      method_name{"compute"};
        DAS::ExportInterface::DasVariantVector result;

        auto start = std::chrono::steady_clock::now();
        component->Dispatch(method_name.Get(), params.Get(), result.Put());
        auto end = std::chrono::steady_clock::now();

        double latency_us =
            std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(latency_us);
    }

    double mean = das::benchmark::CalculateMean(latencies);
    double p50 = das::benchmark::CalculatePercentile(latencies, 50);
    double p95 = das::benchmark::CalculatePercentile(latencies, 95);
    double p99 = das::benchmark::CalculatePercentile(latencies, 99);
    auto [min_it, max_it] =
        std::minmax_element(latencies.begin(), latencies.end());
    double min_val = *min_it;
    double max_val = *max_it;
    double total_s = mean * kIterations / 1e6;
    double throughput = static_cast<double>(kIterations) / total_s;

    PrintBenchmarkResult(
        "RoundTripLatency_Compute",
        kIterations,
        "compute(\"add\", 42, 58) ~26 bytes",
        mean,
        p50,
        p95,
        p99,
        min_val,
        max_val,
        throughput);
}

// ====== Task 4: RoundTripLatency_Echo_SmallString ======

TEST_F(IpcPerformanceTest, RoundTripLatency_Echo_SmallString)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found";
    }

    auto launcher = StartSingleHost();
    if (!launcher)
    {
        GTEST_SKIP() << "Failed to start Host process";
    }

    auto component = LoadTestPlugin2(launcher.Get());
    if (!component)
    {
        GTEST_SKIP() << "Failed to load IpcTestPlugin2";
    }

    constexpr size_t kWarmup = 100;
    constexpr size_t kIterations = 1000;

    // Pre-construct string outside measurement loop
    std::string small_string(32, 'a');
    auto        params = CreateStringParam(small_string);

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i)
    {
        DasReadOnlyString                      method_name{"echo"};
        DAS::ExportInterface::DasVariantVector result;
        component->Dispatch(method_name.Get(), params.Get(), result.Put());
    }

    SuppressLogDuringBenchmark log_guard;

    std::vector<double> latencies;
    latencies.reserve(kIterations);

    for (size_t i = 0; i < kIterations; ++i)
    {
        DasReadOnlyString                      method_name{"echo"};
        DAS::ExportInterface::DasVariantVector result;

        auto start = std::chrono::steady_clock::now();
        component->Dispatch(method_name.Get(), params.Get(), result.Put());
        auto end = std::chrono::steady_clock::now();

        double latency_us =
            std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(latency_us);
    }

    double mean = das::benchmark::CalculateMean(latencies);
    double p50 = das::benchmark::CalculatePercentile(latencies, 50);
    double p95 = das::benchmark::CalculatePercentile(latencies, 95);
    double p99 = das::benchmark::CalculatePercentile(latencies, 99);
    auto [min_it, max_it] =
        std::minmax_element(latencies.begin(), latencies.end());
    double min_val = *min_it;
    double max_val = *max_it;
    double total_s = mean * kIterations / 1e6;
    double throughput = static_cast<double>(kIterations) / total_s;

    PrintBenchmarkResult(
        "RoundTripLatency_Echo_SmallString",
        kIterations,
        "echo(32 bytes) ~44 bytes (with \"[Cpp] echo: \" prefix)",
        mean,
        p50,
        p95,
        p99,
        min_val,
        max_val,
        throughput);
}

// ====== Task 4: RoundTripLatency_Echo_MediumString ======

TEST_F(IpcPerformanceTest, RoundTripLatency_Echo_MediumString)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found";
    }

    auto launcher = StartSingleHost();
    if (!launcher)
    {
        GTEST_SKIP() << "Failed to start Host process";
    }

    auto component = LoadTestPlugin2(launcher.Get());
    if (!component)
    {
        GTEST_SKIP() << "Failed to load IpcTestPlugin2";
    }

    constexpr size_t kWarmup = 100;
    constexpr size_t kIterations = 1000;

    // Pre-construct 4KB string outside measurement loop
    std::string medium_string(4096, 'b');
    auto        params = CreateStringParam(medium_string);

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i)
    {
        DasReadOnlyString                      method_name{"echo"};
        DAS::ExportInterface::DasVariantVector result;
        component->Dispatch(method_name.Get(), params.Get(), result.Put());
    }

    SuppressLogDuringBenchmark log_guard;

    std::vector<double> latencies;
    latencies.reserve(kIterations);

    for (size_t i = 0; i < kIterations; ++i)
    {
        DasReadOnlyString                      method_name{"echo"};
        DAS::ExportInterface::DasVariantVector result;

        auto start = std::chrono::steady_clock::now();
        component->Dispatch(method_name.Get(), params.Get(), result.Put());
        auto end = std::chrono::steady_clock::now();

        double latency_us =
            std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(latency_us);
    }

    double mean = das::benchmark::CalculateMean(latencies);
    double p50 = das::benchmark::CalculatePercentile(latencies, 50);
    double p95 = das::benchmark::CalculatePercentile(latencies, 95);
    double p99 = das::benchmark::CalculatePercentile(latencies, 99);
    auto [min_it, max_it] =
        std::minmax_element(latencies.begin(), latencies.end());
    double min_val = *min_it;
    double max_val = *max_it;
    double total_s = mean * kIterations / 1e6;
    double throughput = static_cast<double>(kIterations) / total_s;

    PrintBenchmarkResult(
        "RoundTripLatency_Echo_MediumString",
        kIterations,
        "echo(4096 bytes) ~4108 bytes (with \"[Cpp] echo: \" prefix)",
        mean,
        p50,
        p95,
        p99,
        min_val,
        max_val,
        throughput);
}

// ====== Task 5: RoundTripLatency_Echo_LargePayload_32KB ======

TEST_F(IpcPerformanceTest, RoundTripLatency_Echo_LargePayload_32KB)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found";
    }

    auto launcher = StartSingleHost();
    if (!launcher)
    {
        GTEST_SKIP() << "Failed to start Host process";
    }

    auto component = LoadTestPlugin2(launcher.Get());
    if (!component)
    {
        GTEST_SKIP() << "Failed to load IpcTestPlugin2";
    }

    constexpr size_t kWarmup = 100;
    constexpr size_t kIterations = 500;

    // Pre-construct 32000-byte string outside measurement loop
    std::string large_string(32000, 'c');
    auto        params = CreateStringParam(large_string);

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i)
    {
        DasReadOnlyString                      method_name{"echo"};
        DAS::ExportInterface::DasVariantVector result;
        component->Dispatch(method_name.Get(), params.Get(), result.Put());
    }

    SuppressLogDuringBenchmark log_guard;

    std::vector<double> latencies;
    latencies.reserve(kIterations);

    for (size_t i = 0; i < kIterations; ++i)
    {
        DasReadOnlyString                      method_name{"echo"};
        DAS::ExportInterface::DasVariantVector result;

        auto start = std::chrono::steady_clock::now();
        component->Dispatch(method_name.Get(), params.Get(), result.Put());
        auto end = std::chrono::steady_clock::now();

        double latency_us =
            std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(latency_us);
    }

    double mean = das::benchmark::CalculateMean(latencies);
    double p50 = das::benchmark::CalculatePercentile(latencies, 50);
    double p95 = das::benchmark::CalculatePercentile(latencies, 95);
    double p99 = das::benchmark::CalculatePercentile(latencies, 99);
    auto [min_it, max_it] =
        std::minmax_element(latencies.begin(), latencies.end());
    double min_val = *min_it;
    double max_val = *max_it;
    double total_s = mean * kIterations / 1e6;
    double throughput = static_cast<double>(kIterations) / total_s;

    PrintBenchmarkResult(
        "RoundTripLatency_Echo_LargePayload_32KB",
        kIterations,
        "echo(32000 bytes) ~32012 bytes (with \"[Cpp] echo: \" prefix)",
        mean,
        p50,
        p95,
        p99,
        min_val,
        max_val,
        throughput);
}

// ====== Task 6: ColdStart_FirstCall ======
// Uses TEST (not TEST_F) because each iteration needs a full
// context → host → measure → teardown lifecycle.

TEST(IpcPerfColdStart, ColdStart_FirstCall)
{
    // 设置 Host 进程日志等级为 warn，抑制 info 级别日志
#ifdef DAS_WINDOWS
    _putenv_s("DAS_LOG_LEVEL", "warn");
#else
    setenv("DAS_LOG_LEVEL", "warn", 1);
#endif

    const std::string host_exe_path = IpcTestConfig::GetDasHostPath();

    if (!std::filesystem::exists(host_exe_path))
    {
        GTEST_SKIP() << "DasHostX.exe not found";
    }

    constexpr size_t    kCycles = 10;
    std::vector<double> first_call_latencies;
    first_call_latencies.reserve(kCycles);

    for (size_t cycle = 0; cycle < kCycles; ++cycle)
    {
        // 1. Create IIpcContext
        auto ctx = DAS::Core::IPC::MainProcess::CreateIpcContextShared(
            /*enable_heartbeat=*/!IpcTestConfig::ShouldDisableHeartbeat());
        if (!ctx)
        {
            continue;
        }

        // 2. Start event loop thread
        std::thread run_thread([&ctx]() { ctx->Run(); });

        // Wait for event loop to start
        for (int i = 0; i < 100 && !ctx->GetIoContext().stopped(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // 3. Create and start Host
        DAS::Core::IPC::IHostLauncher* raw_launcher = nullptr;
        DasResult result = ctx->CreateHostLauncher(&raw_launcher);
        if (DAS::IsFailed(result) || !raw_launcher)
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }
        DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher(raw_launcher);

        uint16_t session_id = 0;
        result = launcher->Start(
            host_exe_path,
            session_id,
            IpcTestConfig::GetHostStartTimeoutMs());
        if (DAS::IsFailed(result))
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }

        // 4. Load IpcTestPlugin2
        std::string plugin_path;
        try
        {
            plugin_path =
                IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
        }
        catch (const std::exception&)
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }

        DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
        result =
            ctx->LoadPluginAsync(launcher.Get(), plugin_path.c_str(), op.Put());
        if (DAS::IsFailed(result))
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }

        auto opt = DAS::Core::IPC::wait(
            *ctx,
            DAS::Core::IPC::async_op(*ctx, std::move(op)));
        if (!opt.has_value())
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }

        auto& [load_result, proxy] = *opt;
        if (DAS::IsFailed(load_result) || !proxy)
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }

        DAS::DasPtr<IDasBase> raw_proxy;
        raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
        if (!raw_proxy)
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }

        DasPluginPackage plugin_package;
        if (DAS::IsFailed(raw_proxy.As(plugin_package.Put())))
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }

        DasPluginFeature feature;
        if (DAS::IsFailed(plugin_package->EnumFeature(0, &feature)))
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }

        IDasBase* factory_base_raw = nullptr;
        if (DAS::IsFailed(
                plugin_package->CreateFeatureInterface(0, &factory_base_raw)))
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }
        DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

        DAS::DasPtr<IDasComponentFactory> factory;
        if (DAS::IsFailed(factory_base.As(factory.Put())))
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }

        if (DAS::IsFailed(factory->IsSupported(DasIidOf<IDasComponent>())))
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }

        IDasComponent* component_raw = nullptr;
        if (DAS::IsFailed(factory->CreateInstance(
                DasIidOf<IDasComponent>(),
                &component_raw)))
        {
            ctx->RequestStop();
            run_thread.join();
            continue;
        }
        DAS::DasPtr<IDasComponent> component(component_raw);

        // 5. Measure first Dispatch call
        {
            SuppressLogDuringBenchmark log_guard;

            DasReadOnlyString                      method_name{"compute"};
            DAS::ExportInterface::DasVariantVector result;
            // Inline CreateComputeParams (fixture method unavailable in TEST)
            DAS::ExportInterface::DasVariantVector params;
            CreateIDasVariantVector(params.Put());
            DasReadOnlyString op_str{"add"};
            params->PushBackString(op_str.Get());
            params->PushBackInt(1);
            params->PushBackInt(1);

            auto start = std::chrono::steady_clock::now();
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
            auto end = std::chrono::steady_clock::now();

            double latency_us =
                std::chrono::duration<double, std::micro>(end - start).count();
            first_call_latencies.push_back(latency_us);
        }

        // 6. Teardown
        // launcher 必须在 ctx 之前析构：HostLauncher::Stop() 内部需要
        // io_context 存活才能安全清理 boost::process 的 wait callback。
        launcher.Reset();
        ctx->RequestStop();
        run_thread.join();
        ctx.reset();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (first_call_latencies.empty())
    {
        GTEST_SKIP() << "No successful cold start cycles completed";
    }

    double mean = das::benchmark::CalculateMean(first_call_latencies);
    double p50 = das::benchmark::CalculatePercentile(first_call_latencies, 50);
    double p95 = das::benchmark::CalculatePercentile(first_call_latencies, 95);
    double p99 = das::benchmark::CalculatePercentile(first_call_latencies, 99);
    auto [min_it, max_it] = std::minmax_element(
        first_call_latencies.begin(),
        first_call_latencies.end());
    double min_val = *min_it;
    double max_val = *max_it;

    std::cout << "\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
    std::cout << "  IPC Performance: ColdStart_FirstCall\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
    std::cout << "  Successful Cycles: " << first_call_latencies.size() << "/"
              << kCycles << "\n";
    std::cout << "  Payload:          compute(\"add\", 1, 1) — first call "
                 "after full Host startup\n";
    std::cout << "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n";
    std::cout << "  Mean Latency:   " << std::fixed << std::setprecision(2)
              << mean << " us\n";
    std::cout << "  Median (p50):   " << p50 << " us\n";
    std::cout << "  p95:            " << p95 << " us\n";
    std::cout << "  p99:            " << p99 << " us\n";
    std::cout << "  Min:            " << min_val << " us\n";
    std::cout << "  Max:            " << max_val << " us\n";
    std::cout << "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n";
    std::cout << "  Note: Cold start includes Host launch + IPC handshake + "
                 "plugin load\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
}

// ====== Task 7: Throughput_Sustained_10s ======

TEST_F(IpcPerformanceTest, Throughput_Sustained_10s)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found";
    }

    auto launcher = StartSingleHost();
    if (!launcher)
    {
        GTEST_SKIP() << "Failed to start Host process";
    }

    auto component = LoadTestPlugin2(launcher.Get());
    if (!component)
    {
        GTEST_SKIP() << "Failed to load IpcTestPlugin2";
    }

    constexpr size_t kWarmup = 100;

    // Pre-construct small string for sustained load
    std::string small_string(32, 'a');
    auto        params = CreateStringParam(small_string);

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i)
    {
        DasReadOnlyString                      method_name{"echo"};
        DAS::ExportInterface::DasVariantVector result;
        component->Dispatch(method_name.Get(), params.Get(), result.Put());
    }

    SuppressLogDuringBenchmark log_guard;

    // Sustained load for 10 seconds
    constexpr auto   kDuration = std::chrono::seconds(10);
    constexpr size_t kSampleInterval = 1000;

    std::vector<double> sampled_latencies;
    size_t              total_calls = 0;

    auto deadline = std::chrono::steady_clock::now() + kDuration;

    while (std::chrono::steady_clock::now() < deadline)
    {
        // Run a batch of calls
        for (size_t batch = 0; batch < kSampleInterval; ++batch)
        {
            DasReadOnlyString                      method_name{"echo"};
            DAS::ExportInterface::DasVariantVector result;

            auto start = std::chrono::steady_clock::now();
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
            auto end = std::chrono::steady_clock::now();

            // Check if we've exceeded the deadline mid-batch
            if (end >= deadline)
            {
                total_calls += batch + 1;
                double latency_us =
                    std::chrono::duration<double, std::micro>(end - start)
                        .count();
                sampled_latencies.push_back(latency_us);
                goto done;
            }

            total_calls++;
        }

        // Record one sample per batch for latency distribution
        {
            DasReadOnlyString                      method_name{"echo"};
            DAS::ExportInterface::DasVariantVector result;
            auto start = std::chrono::steady_clock::now();
            component->Dispatch(method_name.Get(), params.Get(), result.Put());
            auto   end = std::chrono::steady_clock::now();
            double latency_us =
                std::chrono::duration<double, std::micro>(end - start).count();
            sampled_latencies.push_back(latency_us);
            total_calls++;
        }
    }

done:

    if (sampled_latencies.empty())
    {
        GTEST_SKIP() << "No samples collected during sustained load";
    }

    double mean = das::benchmark::CalculateMean(sampled_latencies);
    double p50 = das::benchmark::CalculatePercentile(sampled_latencies, 50);
    double p95 = das::benchmark::CalculatePercentile(sampled_latencies, 95);
    double p99 = das::benchmark::CalculatePercentile(sampled_latencies, 99);
    auto [min_it, max_it] =
        std::minmax_element(sampled_latencies.begin(), sampled_latencies.end());
    double min_val = *min_it;
    double max_val = *max_it;
    double throughput = static_cast<double>(total_calls) / 10.0;

    std::cout << "\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
    std::cout << "  IPC Performance: Throughput_Sustained_10s\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
    std::cout << "  Total Calls:    " << total_calls << "\n";
    std::cout << "  Duration:       10s\n";
    std::cout << "  Payload:        echo(32 bytes)\n";
    std::cout << "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n";
    std::cout << "  Avg Throughput: " << std::fixed << std::setprecision(1)
              << throughput << " ops/sec\n";
    std::cout << "  Mean Latency:   " << std::fixed << std::setprecision(2)
              << mean << " us (sampled)\n";
    std::cout << "  Median (p50):   " << p50 << " us\n";
    std::cout << "  p95:            " << p95 << " us\n";
    std::cout << "  p99:            " << p99 << " us\n";
    std::cout << "  Min:            " << min_val << " us\n";
    std::cout << "  Max (spike):    " << max_val << " us\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
}

// ====== Task 7b: ReverseIPC_QueryMainProcessString ======

TEST_F(IpcPerformanceTest, ReverseIPC_QueryMainProcessString)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found";
    }

    // 1. Register IDasReadOnlyString service in main process
    DAS::DasPtr<IDasReadOnlyString> service_string;
    DasResult create_result = CreateIDasReadOnlyStringFromUtf8(
        "Hello from IPC Performance Test",
        service_string.Put());
    if (DAS::IsFailed(create_result))
    {
        GTEST_SKIP() << "Failed to create test string";
    }

    DasResult result = ctx_->RegisterService(
        service_string.Get(),
        DasIidOf<IDasReadOnlyString>());
    if (DAS::IsFailed(result))
    {
        GTEST_SKIP() << "RegisterService failed";
    }

    // 2. Start Host
    auto launcher = StartSingleHost();
    if (!launcher)
    {
        GTEST_SKIP() << "Failed to start Host process";
    }

    // 3. Load IpcTestPlugin1 (not Plugin2 — queryMainProcessString is in
    // Plugin1)
    auto component = LoadTestPlugin1(launcher.Get());
    if (!component)
    {
        GTEST_SKIP() << "Failed to load IpcTestPlugin1";
    }

    constexpr size_t kWarmup = 100;
    constexpr size_t kIterations = 1000;

    // Pre-construct empty params
    DAS::ExportInterface::DasVariantVector empty_params;
    CreateIDasVariantVector(empty_params.Put());

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i)
    {
        DasReadOnlyString method_name{"queryMainProcessString"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        component->Dispatch(
            method_name.Get(),
            empty_params.Get(),
            dispatch_result.Put());
    }

    SuppressLogDuringBenchmark log_guard;

    std::vector<double> latencies;
    latencies.reserve(kIterations);

    for (size_t i = 0; i < kIterations; ++i)
    {
        DasReadOnlyString method_name{"queryMainProcessString"};
        DAS::ExportInterface::DasVariantVector dispatch_result;

        auto start = std::chrono::steady_clock::now();
        component->Dispatch(
            method_name.Get(),
            empty_params.Get(),
            dispatch_result.Put());
        auto end = std::chrono::steady_clock::now();

        double latency_us =
            std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(latency_us);
    }

    double mean = das::benchmark::CalculateMean(latencies);
    double p50 = das::benchmark::CalculatePercentile(latencies, 50);
    double p95 = das::benchmark::CalculatePercentile(latencies, 95);
    double p99 = das::benchmark::CalculatePercentile(latencies, 99);
    auto [min_it, max_it] =
        std::minmax_element(latencies.begin(), latencies.end());
    double min_val = *min_it;
    double max_val = *max_it;
    double total_s = mean * kIterations / 1e6;
    double throughput = static_cast<double>(kIterations) / total_s;

    PrintBenchmarkResult(
        "ReverseIPC_QueryMainProcessString [Host \xE2\x86\x92 MainProcess]",
        kIterations,
        "queryMainProcessString — Reverse IPC: Host queries MainProcess string",
        mean,
        p50,
        p95,
        p99,
        min_val,
        max_val,
        throughput);
}

TEST_F(IpcPerformanceTest, MixedTransport_HttpAndIpcHostToHostPerformance)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found";
    }

    ASSERT_GT(http_port_, static_cast<uint16_t>(0))
        << "SetUp did not create an HTTP-enabled MainProcess context";

    auto ipc_launcher = StartSingleHost();
    ASSERT_NE(ipc_launcher.Get(), nullptr) << "Failed to start IPC Host";

    const uint16_t ipc_session = ipc_launcher->GetSessionId();
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

    auto ipc_package = LoadPluginPackageForHost(
        ipc_launcher.Get(),
        ipc_plugin_path,
        /*attempts=*/1);
    ASSERT_NE(ipc_package.Get(), nullptr);

    auto ipc_factory =
        GetComponentFactoryFromPackage(ipc_package.Get(), /*feature_index=*/1);
    ASSERT_NE(ipc_factory.Get(), nullptr);

    DAS::PluginInterface::IDasComponent* component_raw = nullptr;
    ASSERT_EQ(
        ipc_factory->CreateInstance(DasIidOf<IDasComponent>(), &component_raw),
        DAS_S_OK);
    DAS::DasPtr<IDasComponent> component(component_raw);
    ASSERT_NE(component.Get(), nullptr);

    DAS::ExportInterface::DasVariantVector empty_params;
    ASSERT_EQ(CreateIDasVariantVector(empty_params.Put()), DAS_S_OK);

    DasReadOnlyString method_name{"queryMainProcessString"};

    auto invoke_mixed_dispatch = [&](double* latency_us) -> DasResult
    {
        DAS::DasPtr<IDasReadOnlyString> forwarding_service =
            DAS::DasPtr<IDasReadOnlyString>::Attach(
                new ForwardingReadOnlyString(http_remote_name));

        DasResult call_result = ctx_->RegisterService(
            forwarding_service.Get(),
            DasIidOf<IDasReadOnlyString>());
        if (DAS::IsFailed(call_result))
        {
            return call_result;
        }

        DAS::ExportInterface::DasVariantVector dispatch_result;
        auto start = std::chrono::steady_clock::now();
        call_result = component->Dispatch(
            method_name.Get(),
            empty_params.Get(),
            dispatch_result.Put());
        auto end = std::chrono::steady_clock::now();

        DasResult unregister_result =
            ctx_->UnregisterService(DasIidOf<IDasReadOnlyString>());
        if (DAS::IsFailed(unregister_result) && DAS::IsOk(call_result))
        {
            call_result = unregister_result;
        }

        if (latency_us)
        {
            *latency_us =
                std::chrono::duration<double, std::micro>(end - start).count();
        }

        return call_result;
    };

    constexpr size_t kWarmup = 50;
    constexpr size_t kIterations = 500;

    for (size_t i = 0; i < kWarmup; ++i)
    {
        ASSERT_EQ(invoke_mixed_dispatch(nullptr), DAS_S_OK);
    }

    SuppressLogDuringBenchmark log_guard;

    std::vector<double> latencies;
    latencies.reserve(kIterations);

    for (size_t i = 0; i < kIterations; ++i)
    {
        double    latency_us = 0.0;
        DasResult result = invoke_mixed_dispatch(&latency_us);
        ASSERT_EQ(result, DAS_S_OK);
        latencies.push_back(latency_us);
    }

    double mean = das::benchmark::CalculateMean(latencies);
    double p50 = das::benchmark::CalculatePercentile(latencies, 50);
    double p95 = das::benchmark::CalculatePercentile(latencies, 95);
    double p99 = das::benchmark::CalculatePercentile(latencies, 99);
    auto [min_it, max_it] =
        std::minmax_element(latencies.begin(), latencies.end());
    double min_val = *min_it;
    double max_val = *max_it;
    double total_s = mean * kIterations / 1e6;
    double throughput = static_cast<double>(kIterations) / total_s;

    PrintBenchmarkResult(
        "MixedTransport_HttpAndIpcHostToHostPerformance",
        kIterations,
        "queryMainProcessString dispatch, refreshed forwarding service",
        mean,
        p50,
        p95,
        p99,
        min_val,
        max_val,
        throughput);

    http_host.Stop();
    ipc_launcher->Stop();
}

// ====== Task 7c: RemoteProxy_IsSupported_FirstCall ======

TEST_F(IpcPerformanceTest, RemoteProxy_IsSupported_FirstCall)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found";
    }

    auto launcher = StartSingleHost();
    if (!launcher)
    {
        GTEST_SKIP() << "Failed to start Host process";
    }

    // Load IpcTestPlugin2 and get IDasComponentFactory
    std::string plugin_path;
    try
    {
        plugin_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception&)
    {
        GTEST_SKIP() << "Plugin JSON not found";
    }

    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    DasResult                                 result =
        ctx_->LoadPluginAsync(launcher.Get(), plugin_path.c_str(), op.Put());
    if (DAS::IsFailed(result))
    {
        GTEST_SKIP() << "LoadPluginAsync failed";
    }

    auto opt = DAS::Core::IPC::wait(
        GetContext(),
        DAS::Core::IPC::async_op(GetContext(), std::move(op)));
    if (!opt.has_value())
    {
        GTEST_SKIP() << "wait(async_op) failed";
    }

    auto& [load_result, proxy] = *opt;
    if (DAS::IsFailed(load_result) || !proxy)
    {
        GTEST_SKIP() << "Plugin load failed";
    }

    DAS::DasPtr<IDasBase> raw_proxy;
    raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
    if (!raw_proxy)
    {
        GTEST_SKIP() << "Attach proxy failed";
    }

    DasPluginPackage plugin_package;
    if (DAS::IsFailed(raw_proxy.As(plugin_package.Put())))
    {
        GTEST_SKIP() << "As(DasPluginPackage) failed";
    }

    // Get factory via EnumFeature(0) + CreateFeatureInterface(0)
    DasPluginFeature feature;
    if (DAS::IsFailed(plugin_package->EnumFeature(0, &feature)))
    {
        GTEST_SKIP() << "EnumFeature failed";
    }

    // Warmup: 100 iterations of IsSupported (re-using same factory proxy)
    {
        IDasBase* factory_base_raw = nullptr;
        if (DAS::IsFailed(
                plugin_package->CreateFeatureInterface(0, &factory_base_raw)))
        {
            GTEST_SKIP() << "CreateFeatureInterface failed";
        }
        DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

        DAS::DasPtr<IDasComponentFactory> factory;
        if (DAS::IsFailed(factory_base.As(factory.Put())))
        {
            GTEST_SKIP() << "As(IDasComponentFactory) failed";
        }

        for (size_t i = 0; i < 100; ++i)
        {
            factory->IsSupported(DasIidOf<IDasComponent>());
        }
    }

    SuppressLogDuringBenchmark log_guard;

    constexpr size_t    kIterations = 1000;
    std::vector<double> latencies;
    latencies.reserve(kIterations);

    for (size_t i = 0; i < kIterations; ++i)
    {
        // Each measurement: get factory proxy + call IsSupported
        IDasBase* factory_base_raw = nullptr;
        plugin_package->CreateFeatureInterface(0, &factory_base_raw);
        DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

        DAS::DasPtr<IDasComponentFactory> factory;
        factory_base.As(factory.Put());

        auto start = std::chrono::steady_clock::now();
        factory->IsSupported(DasIidOf<IDasComponent>());
        auto end = std::chrono::steady_clock::now();

        double latency_us =
            std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(latency_us);
    }

    double mean = das::benchmark::CalculateMean(latencies);
    double p50 = das::benchmark::CalculatePercentile(latencies, 50);
    double p95 = das::benchmark::CalculatePercentile(latencies, 95);
    double p99 = das::benchmark::CalculatePercentile(latencies, 99);
    auto [min_it, max_it] =
        std::minmax_element(latencies.begin(), latencies.end());
    double min_val = *min_it;
    double max_val = *max_it;
    double total_s = mean * kIterations / 1e6;
    double throughput = static_cast<double>(kIterations) / total_s;

    PrintBenchmarkResult(
        "RemoteProxy_IsSupported_FirstCall [GUID comparison]",
        kIterations,
        "IsSupported(DasIidOf<IDasComponent>) — RemoteProxy: GUID lookup",
        mean,
        p50,
        p95,
        p99,
        min_val,
        max_val,
        throughput);
}

// ====== Task 8: Stress_HighFrequency_Dispatch ======

TEST_F(IpcPerformanceTest, Stress_HighFrequency_Dispatch)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found";
    }

    auto launcher = StartSingleHost();
    if (!launcher)
    {
        GTEST_SKIP() << "Failed to start Host process";
    }

    auto component = LoadTestPlugin2(launcher.Get());
    if (!component)
    {
        GTEST_SKIP() << "Failed to load IpcTestPlugin2";
    }

    constexpr size_t kWarmup = 100;
    constexpr size_t kIterations = 5000;

    // Pre-construct params
    auto params = CreateComputeParams("add", 1, 1);

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i)
    {
        DasReadOnlyString                      method_name{"compute"};
        DAS::ExportInterface::DasVariantVector result;
        component->Dispatch(method_name.Get(), params.Get(), result.Put());
    }

    // Suppress logs during measurement
    SuppressLogDuringBenchmark log_guard;

    std::vector<double> latencies;
    latencies.reserve(kIterations);

    for (size_t i = 0; i < kIterations; ++i)
    {
        DasReadOnlyString                      method_name{"compute"};
        DAS::ExportInterface::DasVariantVector result;

        auto start = std::chrono::steady_clock::now();
        component->Dispatch(method_name.Get(), params.Get(), result.Put());
        auto end = std::chrono::steady_clock::now();

        double latency_us =
            std::chrono::duration<double, std::micro>(end - start).count();
        latencies.push_back(latency_us);
    }

    double mean = das::benchmark::CalculateMean(latencies);
    double p50 = das::benchmark::CalculatePercentile(latencies, 50);
    double p95 = das::benchmark::CalculatePercentile(latencies, 95);
    double p99 = das::benchmark::CalculatePercentile(latencies, 99);
    auto [min_it, max_it] =
        std::minmax_element(latencies.begin(), latencies.end());
    double min_val = *min_it;
    double max_val = *max_it;
    double total_s = mean * kIterations / 1e6;
    double throughput = static_cast<double>(kIterations) / total_s;

    // Spike ratio: p99/p50 — evaluates BusinessThread serial processing
    // stability under high frequency
    double spike_ratio = (p50 > 0.0) ? (p99 / p50) : 0.0;

    PrintBenchmarkResult(
        "Stress_HighFrequency_Dispatch",
        kIterations,
        "compute(\"add\", 1, 1) — back-to-back, no delay",
        mean,
        p50,
        p95,
        p99,
        min_val,
        max_val,
        throughput);

    std::cout << "  Spike Ratio (p99/p50): " << std::fixed
              << std::setprecision(2) << spike_ratio << "x\n";
    std::cout << "  Note: Higher ratio indicates BusinessThread latency "
                 "accumulation\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
}

// ====== Task 9: DualHost_AlternatingCalls ======

TEST_F(IpcPerformanceTest, DualHost_AlternatingCalls)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found";
    }

    // 1. Start dual hosts
    auto [launcher1, launcher2] = StartDualHost();
    if (!launcher1 || !launcher2)
    {
        GTEST_SKIP() << "Failed to start dual Host processes";
    }

    // 2. Get plugin path
    std::string plugin_path;
    try
    {
        plugin_path = IpcTestConfig::GetTestPluginJsonPath("IpcTestPlugin2");
    }
    catch (const std::exception&)
    {
        GTEST_SKIP() << "Plugin JSON not found";
    }

    // 3. Load plugins concurrently via when_all
    DAS::DasPtr<IDasAsyncLoadPluginOperation> op1, op2;
    DasResult                                 result =
        ctx_->LoadPluginAsync(launcher1.Get(), plugin_path.c_str(), op1.Put());
    if (DAS::IsFailed(result))
    {
        GTEST_SKIP() << "LoadPluginAsync for Host A failed";
    }
    result =
        ctx_->LoadPluginAsync(launcher2.Get(), plugin_path.c_str(), op2.Put());
    if (DAS::IsFailed(result))
    {
        GTEST_SKIP() << "LoadPluginAsync for Host B failed";
    }

    auto both = stdexec::when_all(
        DAS::Core::IPC::async_op(GetContext(), std::move(op1)),
        DAS::Core::IPC::async_op(GetContext(), std::move(op2)));
    auto results = DAS::Core::IPC::wait(GetContext(), std::move(both));
    if (!results.has_value())
    {
        GTEST_SKIP() << "when_all: wait failed";
    }

    auto& [result1, p1, result2, p2] = *results;
    if (DAS::IsFailed(result1))
    {
        GTEST_SKIP() << "Load plugin on Host A failed";
    }
    if (DAS::IsFailed(result2))
    {
        GTEST_SKIP() << "Load plugin on Host B failed";
    }

    // 4. Get IDasComponent for each Host
    auto GetIdasComponent = [&](IDasBase* proxy) -> DAS::DasPtr<IDasComponent>
    {
        auto raw_proxy = DAS::DasPtr<IDasBase>::Attach(proxy);
        if (!raw_proxy)
        {
            return nullptr;
        }

        DAS::PluginInterface::DasPluginPackage plugin_package;
        if (DAS::IsFailed(raw_proxy.As(plugin_package.Put())))
        {
            return nullptr;
        }

        DAS::PluginInterface::DasPluginFeature feature;
        if (DAS::IsFailed(plugin_package->EnumFeature(0, &feature)))
        {
            return nullptr;
        }

        IDasBase* factory_base_raw = nullptr;
        if (DAS::IsFailed(
                plugin_package->CreateFeatureInterface(0, &factory_base_raw)))
        {
            return nullptr;
        }
        DAS::DasPtr<IDasBase> factory_base(factory_base_raw);

        DAS::DasPtr<IDasComponentFactory> factory;
        if (DAS::IsFailed(factory_base.As(factory.Put())))
        {
            return nullptr;
        }

        if (DAS::IsFailed(factory->IsSupported(DasIidOf<IDasComponent>())))
        {
            return nullptr;
        }

        DAS::PluginInterface::IDasComponent* component_raw = nullptr;
        if (DAS::IsFailed(factory->CreateInstance(
                DasIidOf<IDasComponent>(),
                &component_raw)))
        {
            return nullptr;
        }

        return DAS::DasPtr<IDasComponent>(component_raw);
    };

    auto component_a = GetIdasComponent(p1);
    if (!component_a)
    {
        GTEST_SKIP() << "Failed to get IDasComponent for Host A";
    }

    auto component_b = GetIdasComponent(p2);
    if (!component_b)
    {
        GTEST_SKIP() << "Failed to get IDasComponent for Host B";
    }

    // 5. Warmup: 100 alternating calls
    constexpr size_t kWarmup = 100;
    auto             params = CreateComputeParams("add", 1, 1);

    for (size_t i = 0; i < kWarmup; ++i)
    {
        DasReadOnlyString                      method_name{"compute"};
        DAS::ExportInterface::DasVariantVector dispatch_result;
        auto& component = (i % 2 == 0) ? component_a : component_b;
        component->Dispatch(
            method_name.Get(),
            params.Get(),
            dispatch_result.Put());
    }

    // Suppress logs during measurement
    SuppressLogDuringBenchmark log_guard;

    // 6. Measure 1000 alternating calls
    constexpr size_t    kIterations = 1000;
    std::vector<double> latencies_a;
    std::vector<double> latencies_b;
    std::vector<double> all_latencies;
    latencies_a.reserve(kIterations / 2);
    latencies_b.reserve(kIterations / 2);
    all_latencies.reserve(kIterations);

    for (size_t i = 0; i < kIterations; ++i)
    {
        DasReadOnlyString                      method_name{"compute"};
        DAS::ExportInterface::DasVariantVector dispatch_result;

        auto start = std::chrono::steady_clock::now();
        if (i % 2 == 0)
        {
            component_a->Dispatch(
                method_name.Get(),
                params.Get(),
                dispatch_result.Put());
        }
        else
        {
            component_b->Dispatch(
                method_name.Get(),
                params.Get(),
                dispatch_result.Put());
        }
        auto end = std::chrono::steady_clock::now();

        double latency_us =
            std::chrono::duration<double, std::micro>(end - start).count();
        all_latencies.push_back(latency_us);

        if (i % 2 == 0)
        {
            latencies_a.push_back(latency_us);
        }
        else
        {
            latencies_b.push_back(latency_us);
        }
    }

    // 7. Compute statistics
    double mean_a = das::benchmark::CalculateMean(latencies_a);
    double mean_b = das::benchmark::CalculateMean(latencies_b);

    double global_mean = das::benchmark::CalculateMean(all_latencies);
    double global_p50 = das::benchmark::CalculatePercentile(all_latencies, 50);
    double global_p95 = das::benchmark::CalculatePercentile(all_latencies, 95);
    double global_p99 = das::benchmark::CalculatePercentile(all_latencies, 99);
    auto [min_it, max_it] =
        std::minmax_element(all_latencies.begin(), all_latencies.end());
    double min_val = *min_it;
    double max_val = *max_it;
    double total_s = global_mean * kIterations / 1e6;
    double throughput = static_cast<double>(kIterations) / total_s;

    std::cout << "\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
    std::cout << "  IPC Performance: DualHost_AlternatingCalls\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
    std::cout << "  Iterations:     " << kIterations << " (alternating)\n";
    std::cout << "  Host A calls:   " << latencies_a.size() << "\n";
    std::cout << "  Host B calls:   " << latencies_b.size() << "\n";
    std::cout << "  Payload:        compute(\"add\", 1, 1)\n";
    std::cout << "  Mode:           Dual hosts, alternating calls via "
                 "MainProcess IO Thread\n";
    std::cout << "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n";
    std::cout << "  Host A Mean:    " << std::fixed << std::setprecision(2)
              << mean_a << " us\n";
    std::cout << "  Host B Mean:    " << std::fixed << std::setprecision(2)
              << mean_b << " us\n";
    std::cout << "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n";
    std::cout << "  Global Mean:    " << std::fixed << std::setprecision(2)
              << global_mean << " us\n";
    std::cout << "  Global p50:     " << global_p50 << " us\n";
    std::cout << "  Global p95:     " << global_p95 << " us\n";
    std::cout << "  Global p99:     " << global_p99 << " us\n";
    std::cout << "  Min:            " << min_val << " us\n";
    std::cout << "  Max:            " << max_val << " us\n";
    std::cout << "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
                 "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n";
    std::cout << "  Throughput:     " << std::fixed << std::setprecision(1)
              << throughput << " ops/sec\n";
    std::cout << "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550"
                 "\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n";
}
