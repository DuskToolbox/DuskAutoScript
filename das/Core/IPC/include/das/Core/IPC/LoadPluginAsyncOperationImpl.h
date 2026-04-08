#ifndef DAS_CORE_IPC_LOAD_PLUGIN_ASYNC_OPERATION_IMPL_H
#define DAS_CORE_IPC_LOAD_PLUGIN_ASYNC_OPERATION_IMPL_H

#include <atomic>
#include <cstring>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <das/IDasAsyncOperation.h>
#include <stdexec/execution.hpp>
#include <utility>
#include <vector>

DAS_NS_BEGIN

namespace Core
{
    namespace IPC
    {

        //=====================================================================
        // LoadPluginAsyncOperationImpl
        //=====================================================================

        /**
         * @brief LoadPlugin 专用异步操作实现
         *
         * 独立于 AsyncOperationImpl 模板，直接解析 28 字节响应
         * 并通过 CreateRemoteProxy 创建代理对象。
         *
         * @tparam Sender sender 类型（通常是 AwaitResponseSender）
         */
        template <typename Sender>
        class LoadPluginAsyncOperationImpl final
            : public IDasAsyncLoadPluginOperation
        {
            std::atomic<uint32_t> ref_count_{1};
            std::atomic<int32_t>  status_{DAS_ASYNC_STARTED};
            DasResult             result_{DAS_E_UNDEFINED_RETURN_VALUE};
            DasPtr<IDasBase>      proxy_; // uses Attach to take ownership
            DasPtr<IDasAsyncCompletedHandler> handler_;
            MainProcess::IIpcContext*         ctx_; // raw pointer, not owning

            //=================================================================
            // CompletionReceiver
            //=================================================================
            struct CompletionReceiver;
            friend void tag_invoke(
                stdexec::set_value_t,
                CompletionReceiver&&,
                std::tuple<DasResult, std::vector<uint8_t>, uint16_t>) noexcept;
            struct CompletionReceiver
            {
                using receiver_concept = stdexec::receiver_t;
                LoadPluginAsyncOperationImpl* self_;

                friend void tag_invoke(
                    stdexec::set_value_t,
                    CompletionReceiver&& r,
                    std::tuple<DasResult, std::vector<uint8_t>, uint16_t>
                        response) noexcept
                {
                    auto& [code, data, flags] = response;
                    (void)flags;

                    if (DAS::IsOk(code))
                    {
                        // Parse 28 bytes: ObjectId(8) + DasGuid IID(16) +
                        // session_id(2) + version(2)
                        if (data.size() < 28)
                        {
                            r.self_->Complete(DAS_E_IPC_DESERIALIZATION_FAILED);
                            return;
                        }
                        ObjectId object_id{};
                        std::memcpy(&object_id, data.data(), sizeof(ObjectId));
                        DasGuid iid{};
                        std::memcpy(
                            &iid,
                            data.data() + sizeof(ObjectId),
                            sizeof(DasGuid));
                        // session_id [24, 26) and version [26, 28) ignored

                        IDasBase* proxy = nullptr;
                        auto      result = r.self_->ctx_->CreateRemoteProxy(
                            object_id,
                            iid,
                            &proxy);
                        if (DAS::IsOk(result) && proxy)
                        {
                            r.self_->Complete(DAS_S_OK, proxy);
                        }
                        else
                        {
                            r.self_->Complete(DAS_E_IPC_PROXY_CREATION_FAILED);
                        }
                    }
                    else
                    {
                        r.self_->Complete(code);
                    }
                }

                friend void tag_invoke(
                    stdexec::set_stopped_t,
                    CompletionReceiver&& r) noexcept
                {
                    r.self_->Complete(DAS_E_IPC_TIMEOUT);
                }

                friend stdexec::env<> tag_invoke(
                    stdexec::get_env_t,
                    const CompletionReceiver&) noexcept
                {
                    return {};
                }
            };

            using OpState =
                stdexec::connect_result_t<Sender, CompletionReceiver>;
            OpState op_state_;

        public:
            explicit LoadPluginAsyncOperationImpl(
                Sender&&                  sender,
                MainProcess::IIpcContext* ctx)
                : ctx_(ctx), op_state_(
                                 stdexec::connect(
                                     std::forward<Sender>(sender),
                                     CompletionReceiver{this}))
            {
            }

            void Start() { stdexec::start(op_state_); }

            //=================================================================
            // IDasBase
            //=================================================================
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
                if (iid == DasIidOf<IDasAsyncLoadPluginOperation>()
                    || iid == DasIidOf<IDasAsyncOperation>()
                    || iid == DasIidOf<IDasBase>())
                {
                    AddRef();
                    *pp = static_cast<IDasAsyncLoadPluginOperation*>(this);
                    return DAS_S_OK;
                }
                return DAS_E_NO_INTERFACE;
            }

            //=================================================================
            // IDasAsyncOperation
            //=================================================================
            int32_t GetStatus() override
            {
                return status_.load(std::memory_order_acquire);
            }

            DasResult SetCompleted(
                IDasAsyncCompletedHandler* p_handler) override
            {
                handler_ = p_handler;

                // 如果已完成，立即回调
                if (status_.load(std::memory_order_acquire)
                    != DAS_ASYNC_STARTED)
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

            //=================================================================
            // IDasAsyncLoadPluginOperation
            //=================================================================
            DasResult GetResults(IDasBase** pp_out_plugin) override
            {
                if (!proxy_)
                {
                    *pp_out_plugin = nullptr;
                    return result_;
                }
                proxy_->AddRef();
                *pp_out_plugin = proxy_.Get();
                return result_;
            }

            //=================================================================
            // 内部方法
            //=================================================================

            // Success path: store proxy using Attach (no extra AddRef)
            void Complete(DasResult result, IDasBase* proxy)
            {
                proxy_ = DasPtr<IDasBase>::Attach(proxy);
                result_ = result;
                status_.store(DAS_ASYNC_COMPLETED, std::memory_order_release);
                if (handler_)
                    handler_->OnCompleted(this, DAS_ASYNC_COMPLETED);
            }

            // Error path: no proxy
            void Complete(DasResult result)
            {
                result_ = result;
                status_.store(DAS_ASYNC_FAILED, std::memory_order_release);
                if (handler_)
                    handler_->OnCompleted(this, DAS_ASYNC_FAILED);
            }
        };

        //=====================================================================
        // MakeLoadPluginAsyncOperation — 工厂函数
        //=====================================================================

        /**
         * @brief 创建 LoadPlugin 专用异步操作对象
         *
         * @tparam Sender sender 类型
         * @param sender 要包装的 sender
         * @param ctx IPC 上下文（用于 CreateRemoteProxy）
         * @return DasPtr<IDasAsyncLoadPluginOperation> 异步操作对象
         */
        template <typename Sender>
        inline auto MakeLoadPluginAsyncOperation(
            Sender&&                  sender,
            MainProcess::IIpcContext* ctx)
        {
            using Impl = LoadPluginAsyncOperationImpl<std::decay_t<Sender>>;
            auto* op = new Impl(std::forward<Sender>(sender), ctx);
            op->Start();
            return DasPtr<IDasAsyncLoadPluginOperation>::Attach(op);
        }

    } // namespace IPC
} // namespace Core

DAS_NS_END

#endif // DAS_CORE_IPC_LOAD_PLUGIN_ASYNC_OPERATION_IMPL_H
