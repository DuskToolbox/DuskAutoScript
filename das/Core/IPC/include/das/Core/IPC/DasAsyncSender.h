#ifndef DAS_CORE_IPC_DAS_ASYNC_SENDER_H
#define DAS_CORE_IPC_DAS_ASYNC_SENDER_H

/**
 * @file DasAsyncSender.h
 * @brief IPC 异步操作 stdexec sender 适配器
 *
 * 提供基于 stdexec 的异步操作接口，允许外部代码通过任何提供
 * PostRequest/PumpMessage 接口的类型进行异步操作。
 *
 * 用法：
 * @code
 * auto ctx = MainProcess::CreateIpcContextEz();
 * DasPtr<IDasAsyncLoadPluginOperation> op;
 * ctx->GetServer().SendLoadPluginAsync("plugin.json", session_id, op.Put());
 *
 * // 创建 sender
 * auto sender = DAS::Core::IPC::async_op(*ctx, std::move(op));
 *
 * // 等待完成
 * auto result = DAS::Core::IPC::wait(*ctx, std::move(sender));
 * // result = std::optional<std::tuple<DasResult, ObjectId>>
 * @endcode
 */

#include <das/Core/IPC/ObjectId.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncHandshakeOperation.h>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <optional>
#include <stdexec/execution.hpp>
#include <tuple>
#include <type_traits>
#include <utility>

DAS_NS_BEGIN
namespace Core::IPC
{

    //=============================================================================
    // ScheduleEnv — scheduler 环境（模板化，支持任何 Context 类型）
    //=============================================================================
    template <typename Context>
    struct ScheduleEnv
    {
        Context* ctx;

        friend Context& tag_invoke(
            stdexec::get_completion_scheduler_t<stdexec::set_value_t>,
            const ScheduleEnv& self) noexcept
        {
            return *self.ctx;
        }
    };

    //=============================================================================
    // ScheduleSender — 基础 schedule sender（模板化）
    //=============================================================================
    template <typename Context>
    struct ScheduleSender
    {
        using sender_concept = stdexec::sender_t;
        using completion_signatures =
            stdexec::completion_signatures<stdexec::set_value_t()>;

        Context* ctx;

        template <class Receiver>
        struct OperationState
        {
            Context* ctx;
            Receiver rcvr;
            bool     done{false};

            friend void tag_invoke(
                stdexec::start_t,
                OperationState& self) noexcept
            {
                auto* heap_self =
                    new OperationState{self.ctx, std::move(self.rcvr)};
                heap_self->ScheduleOnContext();
            }

            void ScheduleOnContext()
            {
                ctx->PostRequest(
                    [](void* user_data)
                    {
                        auto* self = static_cast<OperationState*>(user_data);
                        stdexec::set_value(std::move(self->rcvr));
                        self->done = true;
                        delete self;
                    },
                    this);
            }
        };

        template <class Receiver>
        friend auto tag_invoke(
            stdexec::connect_t,
            ScheduleSender self,
            Receiver       rcvr) noexcept -> OperationState<Receiver>
        {
            return {self.ctx, std::move(rcvr)};
        }

        ScheduleEnv<Context> get_env() const noexcept
        {
            return ScheduleEnv<Context>{ctx};
        }
    };

    template <typename Context>
        requires requires(Context& c) {
            c.PostRequest(
                static_cast<void (*)(void*)>(nullptr),
                static_cast<void*>(nullptr));
        }
    ScheduleSender<Context> tag_invoke(
        stdexec::schedule_t,
        Context& ctx) noexcept
    {
        return ScheduleSender<Context>{&ctx};
    }

    //=============================================================================
    // DasAsyncSender — 将 IDasAsyncOperation 包装为 stdexec sender（模板化）
    //=============================================================================

    /**
     * @brief 异步操作到 stdexec sender 的适配器
     *
     * @tparam TAsyncOp 异步操作接口类型（如 IDasAsyncLoadPluginOperation）
     * @tparam TResult 结果类型（如 ObjectId）
     * @tparam Context 上下文类型（需要有 PostRequest 方法）
     *
     * 将基于回调的 IDasAsyncOperation 包装为 stdexec sender，
     * 支持 C++20 协程和 stdexec 组合操作（when_all, let_value 等）。
     *
     * 通常通过 async_op() 工厂函数创建。
     */
    template <typename TAsyncOp, typename TResult, typename Context>
    class DasAsyncSender
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(DasResult, TResult),
            stdexec::set_stopped_t()>;

    private:
        DasPtr<TAsyncOp> op_;
        Context*         ctx_ = nullptr;

        template <typename Receiver>
        struct OperationState
        {
            DasPtr<TAsyncOp> op_;
            Receiver         rcvr_;

            //=====================================================================
            // Handler — 桥接 OnCompleted 回调到 set_value
            //=====================================================================
            struct Handler : IDasAsyncCompletedHandler
            {
                std::atomic<uint32_t> ref_{1};
                DasPtr<TAsyncOp>      op_;
                Receiver              rcvr_;

                Handler(DasPtr<TAsyncOp> op, Receiver rcvr)
                    : op_(std::move(op)), rcvr_(std::move(rcvr))
                {
                }
                uint32_t AddRef() override { return ++ref_; }

                uint32_t Release() override
                {
                    auto r = --ref_;
                    if (r == 0)
                        delete this;
                    return r;
                }

                DasResult QueryInterface(const DasGuid& iid, void** pp) override
                {
                    if (iid == DasIidOf<IDasAsyncCompletedHandler>()
                        || iid == DasIidOf<IDasBase>())
                    {
                        AddRef();
                        *pp = this;
                        return DAS_S_OK;
                    }
                    return DAS_E_NO_INTERFACE;
                }

                DasResult OnCompleted(IDasBase*, int32_t status) noexcept
                    override
                {
                    if (status == DAS_ASYNC_COMPLETED
                        || status == DAS_ASYNC_FAILED)
                    {
                        TResult   value{};
                        DasResult result = op_->GetResults(&value);
                        stdexec::set_value(std::move(rcvr_), result, value);
                    }
                    else
                    {
                        stdexec::set_stopped(std::move(rcvr_));
                    }
                    return DAS_S_OK;
                }
            };

            friend void tag_invoke(
                stdexec::start_t,
                OperationState& self) noexcept
            {
                // 先取裸指针，再 move，避免 use-after-move
                auto* raw_op = self.op_.Get();
                auto* handler =
                    new Handler(std::move(self.op_), std::move(self.rcvr_));
                raw_op->SetCompleted(handler);
                handler->Release(); // SetCompleted 内部会 AddRef
            }
        };

        template <typename Receiver>
        friend auto
        tag_invoke(stdexec::connect_t, DasAsyncSender self, Receiver rcvr)
        {
            return OperationState<Receiver>{
                std::move(self.op_),
                std::move(rcvr)};
        }

    public:
        explicit DasAsyncSender(DasPtr<TAsyncOp> op, Context& ctx)
            : op_(std::move(op)), ctx_(&ctx)
        {
        }

        ScheduleEnv<Context> get_env() const noexcept
        {
            return ScheduleEnv<Context>{ctx_};
        }
    };

    //=============================================================================
    // async_op — 异步操作到 sender 的工厂函数
    //=============================================================================

    /**
     * @brief 将 IDasAsyncLoadPluginOperation 包装为 sender
     *
     * @tparam Context 上下文类型（需要有 PostRequest 方法）
     * @param ctx 上下文引用
     * @param op 异步操作指针
     * @return DasAsyncSender 可用于 wait() 或 stdexec 组合操作
     *
     * @code
     * auto ctx = MainProcess::CreateIpcContextEz();
     * auto sender = async_op(*ctx, std::move(op));
     * auto result = wait(*ctx, std::move(sender));
     * @endcode
     */
    template <typename Context>
    auto async_op(Context& ctx, DasPtr<IDasAsyncLoadPluginOperation> op)
    {
        return DasAsyncSender<IDasAsyncLoadPluginOperation, ObjectId, Context>{
            std::move(op),
            ctx};
    }

    /**
     * @brief 将 IDasAsyncHandshakeOperation 包装为 sender
     */
    template <typename Context>
    auto async_op(Context& ctx, DasPtr<IDasAsyncHandshakeOperation> op)
    {
        return DasAsyncSender<IDasAsyncHandshakeOperation, uint16_t, Context>{
            std::move(op),
            ctx};
    }

    //=============================================================================
    // SyncWaitReceiver — wait() 使用的内部 receiver
    //=============================================================================
    namespace internal
    {
        template <typename ValueTuple>
        struct SyncWaitReceiver
        {
            using receiver_concept = stdexec::receiver_t;
            std::optional<ValueTuple>* result;
            bool*                      done;
            void*                      wakeup;

            template <typename... Ts>
            friend void tag_invoke(
                stdexec::set_value_t,
                SyncWaitReceiver&& self,
                Ts&&... values) noexcept
            {
                self.result->emplace(std::forward<Ts>(values)...);
                *self.done = true;
                if (self.wakeup)
                {
                    auto** wakeup_ptr = static_cast<void**>(self.wakeup);
                    *wakeup_ptr = &self;
                }
            }

            friend void tag_invoke(
                stdexec::set_stopped_t,
                SyncWaitReceiver&& self) noexcept
            {
                *self.done = true;
                if (self.wakeup)
                {
                    auto** wakeup_ptr = static_cast<void**>(self.wakeup);
                    *wakeup_ptr = &self;
                }
            }

            friend stdexec::env<> tag_invoke(
                stdexec::get_env_t,
                const SyncWaitReceiver&) noexcept
            {
                return {};
            }
        };
    } // namespace internal

    template <typename Sender>
    auto wait(Sender&& sender) -> std::optional<stdexec::value_types_of_t<
        std::decay_t<Sender>,
        stdexec::env<>,
        std::tuple,
        std::type_identity_t>>
    {
        using value_tuple_t = stdexec::value_types_of_t<
            std::decay_t<Sender>,
            stdexec::env<>,
            std::tuple,
            std::type_identity_t>;

        std::optional<value_tuple_t> result;
        bool                         done = false;
        void*                        wakeup = nullptr;

        auto op = stdexec::connect(
            std::forward<Sender>(sender),
            internal::SyncWaitReceiver<value_tuple_t>{&result, &done, &wakeup});
        stdexec::start(op);

        return result;
    }

    template <typename Context, typename Sender>
    auto wait(Context& ctx, Sender&& sender)
        -> std::optional<stdexec::value_types_of_t<
            std::decay_t<Sender>,
            stdexec::env<>,
            std::tuple,
            std::type_identity_t>>
    {
        return wait(std::forward<Sender>(sender));
    }

} // namespace Core::IPC

DAS_NS_END

#endif // DAS_CORE_IPC_DAS_ASYNC_SENDER_H
