/**
 * @file HostPostToBusinessThreadTest.cpp
 * @brief Host IIpcContext::PostToBusinessThread +
 * IpcContextEvents::on_after_connect
 *
 * PostToBusinessThread 把 IDasAsyncCallback 投递到 BusinessThread 执行
 * （经 BUSINESS_CONTROL|InternalBusinessCommand::AsyncCallback 消息走
 * inbound_queue_，由 InternalCallbackHandler 解码后调 Do()）。
 * on_after_connect 是连接生命周期事件，照 on_before_shutdown 范式。
 *
 * 构造 Host IpcContext 用 HTTP 模式（connect_url 设值），不调 Run()——
 * BusinessThread 在构造函数内启动并立即开始消费 inbound_queue_，
 * PostToBusinessThread 直接 push 消息即可被 BT 处理，无需网络连接。
 */

#include <atomic>
#include <chrono>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncCallback.h>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <thread>
#include <type_traits>

namespace
{
    /// IDasAsyncCallback 测试桩：记录 Do() 执行线程并通过 promise 同步
    class FlagSettingCallback final : public IDasAsyncCallback
    {
    public:
        explicit FlagSettingCallback(std::function<void()> on_do)
            : on_do_(std::move(on_do))
        {
        }

        uint32_t AddRef() override
        {
            return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        uint32_t Release() override
        {
            const uint32_t count =
                ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (count == 0)
            {
                delete this;
            }
            return count;
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
            *pp_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult Do() override
        {
            thread_id_ = std::this_thread::get_id();
            ran_.store(true, std::memory_order_release);
            if (on_do_)
            {
                on_do_();
            }
            return DAS_S_OK;
        }

        [[nodiscard]]
        bool Ran() const noexcept
        {
            return ran_.load(std::memory_order_acquire);
        }

        [[nodiscard]]
        std::thread::id ThreadId() const noexcept
        {
            return thread_id_;
        }

    private:
        std::atomic<uint32_t> ref_count_{1};
        std::atomic<bool>     ran_{false};
        std::thread::id       thread_id_{};
        std::function<void()> on_do_;
    };

    /// 构造一个不会触发实际连接的 Host IpcContext（HTTP 模式，不调 Run()）
    Das::Core::IPC::Host::IpcContextPtr MakeDisconnectedHostContext()
    {
        Das::Core::IPC::Host::IpcContextConfig config;
        // HTTP 模式：构造函数只解析 URL，不发起连接（连接发生在 Run() 内）
        config.connect_url = "ws://127.0.0.1:1";
        return Das::Core::IPC::Host::CreateIpcContextEz(config);
    }
} // namespace

// ====== Task 1: PostToBusinessThread ======

TEST(HostPostToBusinessThread, CallbackRunsOnBusinessThread)
{
    auto ctx = MakeDisconnectedHostContext();
    ASSERT_NE(ctx, nullptr);

    std::promise<void> done;
    auto               done_future = done.get_future();

    FlagSettingCallback* raw =
        new FlagSettingCallback([&done]() { done.set_value(); });
    // Attach 不 AddRef——raw 的初始引用由本 DasPtr 管理
    Das::DasPtr<IDasAsyncCallback> callback =
        Das::DasPtr<IDasAsyncCallback>::Attach(raw);

    // PostToBusinessThread 内部 AddRef，BT 侧 Attach+Release 配对
    ctx->PostToBusinessThread(callback.Get());

    // 等 Do() 完成（5s 超时防止 hang）
    ASSERT_EQ(
        done_future.wait_for(std::chrono::seconds(5)),
        std::future_status::ready);

    EXPECT_TRUE(raw->Ran());
    // Do() 应在 BusinessThread 上执行，而非测试主线程
    EXPECT_NE(raw->ThreadId(), std::this_thread::get_id());
}

TEST(HostPostToBusinessThread, NullCallbackIsSafe)
{
    auto ctx = MakeDisconnectedHostContext();
    ASSERT_NE(ctx, nullptr);

    // nullptr 不应崩溃，应是安全 no-op
    EXPECT_EQ(ctx->PostToBusinessThread(nullptr), DAS_S_OK);
}
