#ifndef DAS_CORE_IPC_DAS_ASYNC_SENDER_H
#define DAS_CORE_IPC_DAS_ASYNC_SENDER_H

#include <das/Core/IPC/ObjectId.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncHandshakeOperation.h>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <stdexec/execution.hpp>
#include <utility>

DAS_NS_BEGIN

namespace Core::IPC
{

    //=============================================================================
    // DasAsyncSender — 将 IDasAsyncOperation 包装为 stdexec sender
    //=============================================================================

    /**
     * @brief 异步操作到 stdexec sender 的适配器
     *
     * @tparam TAsyncOp 异步操作接口类型（如 IDasAsyncLoadPluginOperation）
     * @tparam TResult 结果类型（如 ObjectId）
     *
     * 将基于回调的 IDasAsyncOperation 包装为 stdexec sender，
     * 支持 C++20 协程和 stdexec 组合操作（when_all, let_value 等）。
     *
     * 使用示例:
     * @code
     * DasPtr<IDasAsyncLoadPluginOperation> op;
     * server.SendLoadPluginAsync("plugin.json", session_id, op.Put());
     * auto [result, object_id] =
     * stdexec::sync_wait(AwaitAsync(std::move(op))).value();
     * @endcode
     */
    template <typename TAsyncOp, typename TResult>
    class DasAsyncSender
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(DasResult, TResult),
            stdexec::set_stopped_t()>;

    private:
        DasPtr<TAsyncOp> op_;

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
                auto* handler =
                    new Handler(std::move(self.op_), std::move(self.rcvr_));
                self.op_->SetCompleted(handler);
                handler->Release(); // op_ 持有引用
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
        explicit DasAsyncSender(DasPtr<TAsyncOp> op) : op_(std::move(op)) {}
    };

    //=============================================================================
    // AwaitAsync — 便捷工厂函数
    //=============================================================================

    /**
     * @brief 将 IDasAsyncLoadPluginOperation 包装为 sender
     */
    inline auto AwaitAsync(DasPtr<IDasAsyncLoadPluginOperation> op)
    {
        return DasAsyncSender<IDasAsyncLoadPluginOperation, ObjectId>{
            std::move(op)};
    }

    /**
     * @brief 将 IDasAsyncHandshakeOperation 包装为 sender
     */
    inline auto AwaitAsync(DasPtr<IDasAsyncHandshakeOperation> op)
    {
        return DasAsyncSender<IDasAsyncHandshakeOperation, uint16_t>{
            std::move(op)};
    }

} // namespace Core::IPC

DAS_NS_END

#endif // DAS_CORE_IPC_DAS_ASYNC_SENDER_H
