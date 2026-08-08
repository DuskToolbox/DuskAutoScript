/**
 * @file LocaleServiceIpcTest.cpp
 * @brief IDasLocaleService 跨进程契约 IPC 端到端测试
 *
 * 验证 das.locale 服务跨进程契约（CONTEXT R4 IPC 往返 + Pitfall 3 refcount
 * 配对 + DasLocaleCallbackImpl 反向通知）：
 *   1. AddCallback 收到的是 callback proxy（非本地对象），QI IPCProxyBase
 * 成功；
 *   2. SetDefaultLocale 反向触发 OnDefaultLocaleChanged（proxy 经 IPC
 * marshalled 回 Host 进程调用本地 callback）；
 *   3. RemoveCallback 清除存储 + 释放 proxy，本地 callback 引用计数回到
 * baseline（DOM 注册与释放成对，无泄漏）；
 *   4. on_after_connect → LocaleSyncCallback 链路间接覆盖（真实 Host 连接
 * 完成，Host 侧 DasLocaleCallbackImpl 也会注册，反向通知到达第二条 callback）。
 *
 * 测试真实启动 DasHostX 进程加载 IpcTestPlugin1，插件侧经
 * DasQueryMainProcessInterfaceByName("das.locale") 拿 service proxy 并执行
 * AddCallback/GetDefaultLocale/SetDefaultLocale/RemoveCallback 往返。
 */

#include <Das.ExportInterface.IDasVariantVector.hpp>
#include <Das.PluginInterface.IDasComponent.hpp>
#include <Das.PluginInterface.IDasComponentFactory.Implements.hpp>
#include <Das.PluginInterface.IDasPluginPackage.hpp>
#include <IpcTestConfig.h>

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/DasTypes.hpp>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <das/IDasBase.h>
#include <das/_autogen/idl/abi/IDasLocaleService.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasLocaleService.Implements.hpp>

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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace EP = Das::ExportInterface;
using Das::DasPtr;
using namespace Das::Core::IPC;

namespace
{
    // QI 到 IPCProxyBase 检测对象是否为跨进程 proxy。
    bool IsProxy(IDasBase* object)
    {
        if (object == nullptr)
        {
            return false;
        }
        IPCProxyBase*   runtime_tag = nullptr;
        const DasResult qi_result = object->QueryInterface(
            DasIidOf<IPCProxyBase>(),
            reinterpret_cast<void**>(&runtime_tag));
        if (DAS::IsOk(qi_result) && runtime_tag != nullptr)
        {
            static_cast<void>(runtime_tag->ReleaseRuntimeTagRef());
            return true;
        }
        return false;
    }

    // 主进程侧 fake IDasLocaleService：记录 AddCallback/RemoveCallback 收到的
    // 对象是否为 proxy + 实现反向通知（SetDefaultLocale 持锁拷贝 callbacks_
    // 副本后锁外遍历，照 DasLocaleServiceImpl 范式）。
    class FakeLocaleService final
        : public EP::DasLocaleServiceImplBase<FakeLocaleService>
    {
    public:
        DasResult AddCallback(EP::IDasLocaleCallback* callback) override
        {
            if (callback == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            const std::lock_guard lock(mutex_);
            add_was_proxy_ = IsProxy(static_cast<IDasBase*>(callback));
            callbacks_.emplace_back(callback);
            return DAS_S_OK;
        }

        DasResult RemoveCallback(EP::IDasLocaleCallback* callback) override
        {
            if (callback == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            const std::lock_guard lock(mutex_);
            const auto            it = std::find_if(
                callbacks_.begin(),
                callbacks_.end(),
                [callback](const DasPtr<EP::IDasLocaleCallback>& stored)
                { return stored.Get() == callback; });
            if (it != callbacks_.end())
            {
                // DasPtr 析构 Release → 释放 callback proxy → 触发远端释放链。
                callbacks_.erase(it);
                remove_matched_ = true;
            }
            return DAS_S_OK;
        }

        DasResult GetDefaultLocale(IDasReadOnlyString** pp_out_locale) override
        {
            if (pp_out_locale == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_locale = nullptr;
            return CreateIDasReadOnlyStringFromUtf8(
                current_locale_.c_str(),
                pp_out_locale);
        }

        DasResult SetDefaultLocale(IDasReadOnlyString* locale) override
        {
            if (locale == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            const char* raw = nullptr;
            if (DAS::IsOk(locale->GetUtf8(&raw)) && raw != nullptr)
            {
                current_locale_ = raw;
            }

            // 反向通知持锁拷贝 callbacks_ 副本后锁外遍历，防重入死锁（照
            // DasLocaleServiceImpl 范式）。
            std::vector<DasPtr<EP::IDasLocaleCallback>> snapshot;
            {
                const std::lock_guard lock(mutex_);
                snapshot = callbacks_;
            }
            for (const auto& cb : snapshot)
            {
                const auto rc = cb->OnDefaultLocaleChanged(locale);
                if (DAS::IsFailed(rc))
                {
                    DAS_LOG_ERROR(
                        DAS_FMT_NS::format(
                            "FakeLocaleService: OnDefaultLocaleChanged "
                            "failed result={}",
                            static_cast<int>(rc))
                            .c_str());
                }
            }
            return DAS_S_OK;
        }

        // ---- accessors (test 侧只读) ----

        [[nodiscard]]
        bool AddWasProxy() const
        {
            const std::lock_guard lock(mutex_);
            return add_was_proxy_;
        }

        [[nodiscard]]
        bool RemoveMatched() const
        {
            const std::lock_guard lock(mutex_);
            return remove_matched_;
        }

        [[nodiscard]]
        size_t CallbackCount() const
        {
            const std::lock_guard lock(mutex_);
            return callbacks_.size();
        }

    private:
        mutable std::mutex                          mutex_;
        std::vector<DasPtr<EP::IDasLocaleCallback>> callbacks_;
        bool                                        add_was_proxy_ = false;
        bool                                        remove_matched_ = false;
        std::string                                 current_locale_ = "en";
    };

    class LocaleServiceIpcE2ETest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            host_exe_path_ = IpcTestConfig::GetDasHostPath();
            ctx_ = MainProcess::CreateIpcContextShared(true);
            ASSERT_NE(ctx_, nullptr);

            IHostLauncher* raw_launcher = nullptr;
            const auto     launcher_result =
                ctx_->CreateHostLauncher(&raw_launcher);
            ASSERT_EQ(launcher_result, DAS_S_OK);
            ASSERT_NE(raw_launcher, nullptr);
            launcher_ = DasPtr<IHostLauncher>(raw_launcher);

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
                ctx_->UnregisterServiceByName("das.locale");
                ctx_->RequestStop();
            }
            if (run_thread_.joinable())
            {
                run_thread_.join();
            }
            launcher_.Reset();
            ctx_.reset();
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

        Das::Core::IPC::MainProcess::IIpcContext& GetContext() { return *ctx_; }

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
                Das::Core::IPC::async_op(GetContext(), std::move(op)));
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
            const DasPtr<Das::PluginInterface::IDasComponentFactory> factory(
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

        std::string                               host_exe_path_;
        std::shared_ptr<MainProcess::IIpcContext> ctx_;
        DasPtr<IHostLauncher>                     launcher_;
        std::thread                               run_thread_;
        bool                                      plugin_missing_ = false;
    };

} // namespace

TEST_F(
    LocaleServiceIpcE2ETest,
    AddCallbackReceivesProxyAndSetDefaultLocaleTriggersReverseNotification)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found at: " << host_exe_path_;
    }

    // 注册 fake service 按名（IDasLocaleService）。
    auto fake_service = FakeLocaleService::Make();
    ASSERT_NE(fake_service.Get(), nullptr);
    auto* fake_raw = static_cast<FakeLocaleService*>(fake_service.Get());
    ASSERT_EQ(
        ctx_->RegisterServiceByName(
            fake_service.Get(),
            DasIidOf<EP::IDasLocaleService>(),
            "das.locale"),
        DAS_S_OK);

    ASSERT_EQ(StartHost(), DAS_S_OK);
    ASSERT_TRUE(launcher_->IsRunning());

    auto component = LoadComponent();
    if (plugin_missing_)
    {
        GTEST_SKIP() << "IpcTestPlugin1 JSON not found";
    }
    ASSERT_NE(component.Get(), nullptr);

    // 插件侧执行 localeServiceRoundTrip：resolve → AddCallback →
    // GetDefaultLocale → SetDefaultLocale（反向通知）→ RemoveCallback → flush。
    const DasReadOnlyString method_name{"localeServiceRoundTrip"};
    EP::DasVariantVector    params;
    EP::DasVariantVector    result;
    ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
    ASSERT_EQ(
        component->Dispatch(method_name.Get(), params.Get(), result.Put()),
        DAS_S_OK);
    ASSERT_NE(result.Get(), nullptr);

    // 插件回传 7 个 int：[resolve_ok, ref_after_create, ref_after_add,
    //                     get_locale_ok, set_locale_ok, change_count_after_set,
    //                     ref_after_flush]
    ASSERT_EQ(result->GetSize(), 7u);
    int64_t resolve_ok = 0;
    int64_t ref_after_create = 0;
    int64_t ref_after_add = 0;
    int64_t get_locale_ok = 0;
    int64_t set_locale_ok = 0;
    int64_t change_count_after_set = 0;
    int64_t ref_after_flush = 0;
    ASSERT_EQ(result->GetInt(0, &resolve_ok), DAS_S_OK);
    ASSERT_EQ(result->GetInt(1, &ref_after_create), DAS_S_OK);
    ASSERT_EQ(result->GetInt(2, &ref_after_add), DAS_S_OK);
    ASSERT_EQ(result->GetInt(3, &get_locale_ok), DAS_S_OK);
    ASSERT_EQ(result->GetInt(4, &set_locale_ok), DAS_S_OK);
    ASSERT_EQ(result->GetInt(5, &change_count_after_set), DAS_S_OK);
    ASSERT_EQ(result->GetInt(6, &ref_after_flush), DAS_S_OK);

    // resolve das.locale 成功。
    EXPECT_EQ(resolve_ok, 1);

    // AddCallback 收到 callback proxy（非本地对象），且 DOM 注册了本地对象
    // （refcount +1）。
    EXPECT_TRUE(fake_raw->AddWasProxy());
    EXPECT_EQ(ref_after_create, 1);
    EXPECT_GT(ref_after_add, ref_after_create);

    // GetDefaultLocale pull 返回 "en"。
    EXPECT_EQ(get_locale_ok, 1);

    // SetDefaultLocale 触发反向通知：插件侧 callback ChangeCount >= 1。
    EXPECT_EQ(set_locale_ok, 1);
    EXPECT_GE(change_count_after_set, 1);

    // RemoveCallback 后本地 callback 引用回到 baseline（refcount 配对归零）。
    EXPECT_TRUE(fake_raw->RemoveMatched());
    EXPECT_EQ(ref_after_flush, ref_after_create);
    // 注意：fake_raw->CallbackCount() 可能 > 0（Host 侧 LocaleSyncCallback
    // 的 DasLocaleCallbackImpl 经 on_after_connect 异步注册，与本 test 共享
    // 同一 fake service）。本断言只验插件 callback 配对，不强制 fake service
    // 计数归零。
}
