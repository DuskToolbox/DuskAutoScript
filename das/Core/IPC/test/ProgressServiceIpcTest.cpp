/**
 * @file ProgressServiceIpcTest.cpp
 * @brief IDasProgressService 跨进程对象参数生命周期 IPC 测试
 *
 * 验证新 IDL 的跨进程 callback/entry 自动代理化与 RemoveCallback DOM 释放
 * （RESEARCH Pitfall 3）：
 *   1. AddCallback 收到的是 callback proxy（非本地对象），QI IPCProxyBase
 * 成功；
 *   2. RemoveCallback 清除存储 + 释放 proxy，本地 callback 引用计数回到
 * baseline （DOM 注册与释放成对，无泄漏）；
 *   3. Report(entry) 经 proxy 逐字段读回（GetKey/GetFieldByIndex 往返）。
 *
 * 测试真实启动 DasHostX 进程加载 IpcTestPlugin1，插件侧经
 * ResolveMainProcessInterfaceByName("Das::ProgressService") 拿 service proxy 并
 * 执行 AddCallback/Report/RemoveCallback 往返。
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
#include <das/_autogen/idl/abi/IDasProgressService.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressService.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressUpdate.Implements.hpp>

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
        IPCProxyBase* runtime_tag = nullptr;
        const auto    qi_result = object->QueryInterface(
            DasIidOf<IPCProxyBase>(),
            reinterpret_cast<void**>(&runtime_tag));
        if (DAS::IsOk(qi_result) && runtime_tag != nullptr)
        {
            static_cast<void>(runtime_tag->ReleaseRuntimeTagRef());
            return true;
        }
        return false;
    }

    // 主进程侧 fake IDasProgressService：记录 AddCallback/RemoveCallback/Report
    // 收到的对象是否为 proxy + 逐字段读回 entry。
    class FakeProgressService final
        : public EP::DasProgressServiceImplBase<FakeProgressService>
    {
    public:
        struct ReportSnapshot
        {
            std::string              key;
            std::vector<std::string> field_names;
            std::vector<std::string> field_values;
        };

        DasResult AddCallback(EP::IDasProgressCallback* callback) override
        {
            if (callback == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            const std::lock_guard lock(mutex_);
            add_received_ = callback;
            add_was_proxy_ = IsProxy(static_cast<IDasBase*>(callback));
            callbacks_.emplace_back(callback);
            return DAS_S_OK;
        }

        DasResult RemoveCallback(EP::IDasProgressCallback* callback) override
        {
            if (callback == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            const std::lock_guard lock(mutex_);
            remove_received_ = callback;
            remove_was_proxy_ = IsProxy(static_cast<IDasBase*>(callback));
            const auto it = std::find_if(
                callbacks_.begin(),
                callbacks_.end(),
                [callback](const DasPtr<EP::IDasProgressCallback>& stored)
                { return stored.Get() == callback; });
            if (it != callbacks_.end())
            {
                // DasPtr 析构 Release → 释放 callback proxy → 触发远端释放链。
                callbacks_.erase(it);
                remove_matched_ = true;
            }
            return DAS_S_OK;
        }

        DasResult Report(EP::IDasProgressUpdate* entry) override
        {
            if (entry == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            const std::lock_guard lock(mutex_);
            ReportSnapshot        snapshot;
            report_was_proxy_ = IsProxy(static_cast<IDasBase*>(entry));

            DasPtr<IDasReadOnlyString> key_str;
            if (DAS::IsOk(entry->GetKey(key_str.Put())) && key_str)
            {
                const char* key_raw = nullptr;
                if (DAS::IsOk(key_str->GetUtf8(&key_raw)) && key_raw)
                {
                    snapshot.key = key_raw;
                }
            }
            size_t field_count = 0;
            if (DAS::IsOk(entry->GetFieldCount(&field_count)))
            {
                for (size_t index = 0; index < field_count; ++index)
                {
                    DasPtr<IDasReadOnlyString> name;
                    DasPtr<EP::IDasJson>       value;
                    if (DAS::IsFailed(entry->GetFieldByIndex(
                            index,
                            name.Put(),
                            value.Put())))
                    {
                        continue;
                    }
                    const char* name_raw = nullptr;
                    if (!name || DAS::IsFailed(name->GetUtf8(&name_raw))
                        || !name_raw)
                    {
                        continue;
                    }
                    DasPtr<IDasReadOnlyString> value_str;
                    if (!value
                        || DAS::IsFailed(value->ToString(0, value_str.Put()))
                        || !value_str)
                    {
                        continue;
                    }
                    const char* value_raw = nullptr;
                    if (DAS::IsFailed(value_str->GetUtf8(&value_raw))
                        || !value_raw)
                    {
                        continue;
                    }
                    snapshot.field_names.emplace_back(name_raw);
                    snapshot.field_values.emplace_back(value_raw);
                }
            }
            reports_.push_back(std::move(snapshot));
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
        bool RemoveWasProxy() const
        {
            const std::lock_guard lock(mutex_);
            return remove_was_proxy_;
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

        [[nodiscard]]
        bool ReportWasProxy() const
        {
            const std::lock_guard lock(mutex_);
            return report_was_proxy_;
        }

        [[nodiscard]]
        const std::vector<ReportSnapshot>& Reports() const
        {
            return reports_;
        }

    private:
        mutable std::mutex        mutex_;
        EP::IDasProgressCallback* add_received_ = nullptr;
        EP::IDasProgressCallback* remove_received_ = nullptr;
        bool                      add_was_proxy_ = false;
        bool                      remove_was_proxy_ = false;
        bool                      remove_matched_ = false;
        bool                      report_was_proxy_ = false;
        std::vector<DasPtr<EP::IDasProgressCallback>> callbacks_;
        std::vector<ReportSnapshot>                   reports_;
    };

    class ProgressServiceIpcE2ETest : public ::testing::Test
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
                ctx_->UnregisterServiceByName("Das::ProgressService");
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
    ProgressServiceIpcE2ETest,
    AddCallbackReceivesProxyAndRemoveCallbackFreesDom)
{
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHostX.exe not found at: " << host_exe_path_;
    }

    // 注册 fake service 按名（IDasProgressService）。
    auto fake_service = FakeProgressService::Make();
    ASSERT_NE(fake_service.Get(), nullptr);
    auto* fake_raw = static_cast<FakeProgressService*>(fake_service.Get());
    ASSERT_EQ(
        ctx_->RegisterServiceByName(
            fake_service.Get(),
            DasIidOf<EP::IDasProgressService>(),
            "Das::ProgressService"),
        DAS_S_OK);

    ASSERT_EQ(StartHost(), DAS_S_OK);
    ASSERT_TRUE(launcher_->IsRunning());

    auto component = LoadComponent();
    if (plugin_missing_)
    {
        GTEST_SKIP() << "IpcTestPlugin1 JSON not found";
    }
    ASSERT_NE(component.Get(), nullptr);

    // 插件侧执行 progressServiceRoundTrip：resolve → AddCallback → Report →
    // RemoveCallback → flush。
    const DasReadOnlyString method_name{"progressServiceRoundTrip"};
    EP::DasVariantVector    params;
    EP::DasVariantVector    result;
    ASSERT_EQ(CreateIDasVariantVector(params.Put()), DAS_S_OK);
    ASSERT_EQ(
        component->Dispatch(method_name.Get(), params.Get(), result.Put()),
        DAS_S_OK);
    ASSERT_NE(result.Get(), nullptr);

    // 插件返回 ref_after_create / ref_after_add / ref_after_flush。
    ASSERT_EQ(result->GetSize(), 3u);
    int64_t ref_after_create = 0;
    int64_t ref_after_add = 0;
    int64_t ref_after_flush = 0;
    ASSERT_EQ(result->GetInt(0, &ref_after_create), DAS_S_OK);
    ASSERT_EQ(result->GetInt(1, &ref_after_add), DAS_S_OK);
    ASSERT_EQ(result->GetInt(2, &ref_after_flush), DAS_S_OK);

    // AddCallback 收到 callback proxy（非本地对象），且 DOM 注册了本地对象
    // （refcount +1）。
    EXPECT_TRUE(fake_raw->AddWasProxy());
    EXPECT_EQ(ref_after_create, 1);
    EXPECT_EQ(ref_after_add, 2);

    // RemoveCallback 后本地 callback 引用回到 baseline（DOM 释放，无泄漏）。
    EXPECT_TRUE(fake_raw->RemoveMatched());
    EXPECT_TRUE(fake_raw->RemoveWasProxy());
    EXPECT_EQ(fake_raw->CallbackCount(), 0u);
    EXPECT_EQ(ref_after_flush, 1);

    // Report(entry) 经 proxy 逐字段读回。
    EXPECT_TRUE(fake_raw->ReportWasProxy());
    const auto& reports = fake_raw->Reports();
    ASSERT_GE(reports.size(), 1u);
    EXPECT_EQ(reports[0].key, "roundtrip-key");
    ASSERT_EQ(reports[0].field_names.size(), 2u);
    EXPECT_EQ(reports[0].field_names[0], "updateProgress");
    EXPECT_EQ(reports[0].field_names[1], "downloadRate");
    ASSERT_EQ(reports[0].field_values.size(), 2u);
    EXPECT_NE(reports[0].field_values[0].find("正在下载"), std::string::npos);
    EXPECT_NE(reports[0].field_values[0].find("50%"), std::string::npos);
    EXPECT_NE(reports[0].field_values[1].find("1.0 MB/s"), std::string::npos);
}
