#ifndef DAS_CORE_IPC_ASYNC_OPERATION_IMPL_H
#define DAS_CORE_IPC_ASYNC_OPERATION_IMPL_H

#include <atomic>
#include <cstring>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasPtr.h>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <das/IDasAsyncOperation.h>
#include <optional>
#include <stdexec/execution.hpp>
#include <utility>
#include <vector>

DAS_NS_BEGIN

namespace Core::IPC
{

    //=============================================================================
    // ResultParser — 结果解析 trait
    //=============================================================================

    /**
     * @brief 结果解析 trait
     *
     * 将 IPC 响应的 vector<uint8_t> 解析为具体类型。
     * 需要为每个 TResult 类型提供特化。
     */
    template <typename TResult>
    struct ResultParser;

    template <>
    struct ResultParser<ObjectId>
    {
        static std::optional<ObjectId> TryParse(
            const std::vector<uint8_t>& data)
        {
            if (data.size() < sizeof(ObjectId))
                return std::nullopt;
            ObjectId result;
            std::memcpy(&result, data.data(), sizeof(ObjectId));
            return result;
        }
    };

    template <>
    struct ResultParser<uint16_t>
    {
        static std::optional<uint16_t> TryParse(
            const std::vector<uint8_t>& data)
        {
            if (data.size() < sizeof(uint16_t))
                return std::nullopt;
            uint16_t result;
            std::memcpy(&result, data.data(), sizeof(uint16_t));
            return result;
        }
    };

    //=============================================================================
    // AsyncOperationImpl — 异步操作实现模板
    //=============================================================================

    /**
     * @brief 异步操作实现模板
     *
     * @tparam TInterface 接口类型（如 IDasAsyncLoadPluginOperation）
     * @tparam TResult 结果类型（如 ObjectId）
     * @tparam Sender sender 类型（通常是 AwaitResponseSender）
     *
     * 将 stdexec sender 包装为 IDasAsyncOperation 接口。
     */
    template <typename TInterface, typename TResult, typename Sender>
    class AsyncOperationImpl : public TInterface
    {
        static_assert(
            std::derived_from<TInterface, IDasAsyncOperation>,
            "TInterface must derive from IDasAsyncOperation");

        std::atomic<uint32_t>             ref_count_{1};
        std::atomic<int32_t>              status_{DAS_ASYNC_STARTED};
        DasResult                         result_{DAS_E_UNDEFINED_RETURN_VALUE};
        TResult                           value_{};
        DasPtr<IDasAsyncCompletedHandler> handler_;

        //=========================================================================
        // CompletionReceiver — 桥接 sender completion 到 Complete()
        //=========================================================================
        struct CompletionReceiver
        {
            using receiver_concept = stdexec::receiver_t;
            AsyncOperationImpl* self_;

            friend void tag_invoke(
                stdexec::set_value_t,
                CompletionReceiver&&                       r,
                std::pair<DasResult, std::vector<uint8_t>> response)
            {
                auto& [code, data] = response;
                TResult value{};

                if (DAS::IsOk(code))
                {
                    auto parsed = ResultParser<TResult>::TryParse(data);
                    if (!parsed)
                    {
                        r.self_->Complete(DAS_E_IPC_DESERIALIZATION_FAILED, {});
                        return;
                    }
                    value = *parsed;
                }
                r.self_->Complete(code, value);
            }

            friend void tag_invoke(
                stdexec::set_stopped_t,
                CompletionReceiver&& r)
            {
                r.self_->Complete(DAS_E_IPC_TIMEOUT, {});
            }

            friend stdexec::empty_env tag_invoke(
                stdexec::get_env_t,
                const CompletionReceiver&) noexcept
            {
                return {};
            }
        };

        using OpState = stdexec::connect_result_t<Sender, CompletionReceiver>;
        OpState op_state_;

    public:
        explicit AsyncOperationImpl(Sender&& sender)
            : op_state_(
                  stdexec::connect(std::move(sender), CompletionReceiver{this}))
        {
        }

        void Start() { stdexec::start(op_state_); }

        //=========================================================================
        // IDasBase
        //=========================================================================
        uint32_t AddRef() override { return ++ref_count_; }

        uint32_t Release() override
        {
            auto r = --ref_count_;
            if (r == 0)
                delete this;
            return r;
        }

        DasResult QueryInterface(const DasGuid& iid, void** pp) override
        {
            if (iid == DasIidOf<TInterface>()
                || iid == DasIidOf<IDasAsyncOperation>()
                || iid == DasIidOf<IDasBase>())
            {
                AddRef();
                *pp = static_cast<TInterface*>(this);
                return DAS_S_OK;
            }
            return DAS_E_NO_INTERFACE;
        }

        //=========================================================================
        // IDasAsyncOperation
        //=========================================================================
        int32_t GetStatus() override
        {
            return status_.load(std::memory_order_acquire);
        }

        DasResult SetCompleted(IDasAsyncCompletedHandler* p_handler) override
        {
            handler_ = p_handler;

            // 如果已完成，立即回调
            if (status_.load(std::memory_order_acquire) != DAS_ASYNC_STARTED)
            {
                if (handler_)
                    handler_->OnCompleted(this, status_.load());
            }
            return DAS_S_OK;
        }

        DasResult Cancel() override
        {
            int32_t expected = DAS_ASYNC_STARTED;
            if (status_.compare_exchange_strong(
                    expected,
                    DAS_ASYNC_CANCELED,
                    std::memory_order_acq_rel))
            {
                result_ = DAS_E_IPC_CANCELED;
                return DAS_S_OK;
            }
            return DAS_E_IPC_INVALID_STATE; // 已完成，无法取消
        }

        //=========================================================================
        // TInterface::GetResults
        //=========================================================================
        DasResult GetResults(TResult* p_out) override
        {
            if (status_.load(std::memory_order_acquire) != DAS_ASYNC_COMPLETED)
                return DAS_E_IPC_INVALID_STATE;
            *p_out = value_;
            return result_;
        }

        //=========================================================================
        // 内部方法
        //=========================================================================
        void Complete(DasResult result, const TResult& value)
        {
            value_ = value;
            result_ = result;

            int32_t new_status =
                DAS::IsOk(result) ? DAS_ASYNC_COMPLETED : DAS_ASYNC_FAILED;
            status_.store(new_status, std::memory_order_release);

            if (handler_)
                handler_->OnCompleted(this, new_status);
        }
    };

    //=============================================================================
    // MakeAsyncOperation — 工厂函数
    //=============================================================================

    /**
     * @brief 创建异步操作对象
     *
     * @tparam TInterface 接口类型
     * @tparam TResult 结果类型
     * @tparam Sender sender 类型
     * @param sender 要包装的 sender
     * @return DasPtr<TInterface> 异步操作对象
     */
    template <typename TInterface, typename TResult, typename Sender>
    DasPtr<TInterface> MakeAsyncOperation(Sender&& sender)
    {
        using Impl =
            AsyncOperationImpl<TInterface, TResult, std::decay_t<Sender>>;
        auto* op = new Impl(std::forward<Sender>(sender));
        op->Start();
        return DasPtr<TInterface>::Attach(op);
    }

} // namespace Core::IPC

DAS_NS_END

#endif // DAS_CORE_IPC_ASYNC_OPERATION_IMPL_H
