#pragma once

#include <das/IDasBase.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        /**
         * @brief Interface for resolving main process services by IID.
         *
         * Inherited by concrete IpcContext implementations (Host::IpcContext
         * and MainProcess::IpcContext), NOT by the public IIpcContext
         * interfaces. Stored as a single thread_local pointer for efficient
         * context lookup.
         */
        struct IResolveContext
        {
            virtual DasResult ResolveMainProcessInterface(
                const DasGuid& iid,
                IDasBase**     pp_out) = 0;
            virtual DasResult RegisterService(
                IDasBase*      p_object,
                const DasGuid& iid) = 0;
            virtual DasResult UnregisterService(const DasGuid& iid) = 0;

            // By-name variants
            virtual DasResult ResolveMainProcessInterfaceByName(
                const char* name,
                IDasBase**  pp_out) = 0;
            virtual DasResult RegisterServiceByName(
                IDasBase*      p_object,
                const DasGuid& iid,
                const char*    name) = 0;
            virtual DasResult UnregisterServiceByName(const char* name) = 0;

        protected:
            ~IResolveContext() = default;
        };

        /**
         * Thread-local IPC context pointer.
         *
         * SAFETY PRECONDITION: Each IpcContext instance must exclusively own
         * its thread. Two IpcContext instances must never share the same
         * thread, otherwise the TLS pointer would be overwritten, causing
         * use-after-free.
         *
         * Current architecture satisfies this:
         * - MainProcess::IpcContext::Run() sets this on the IpcRunLoop's IO
         * thread
         * - Host::IpcContext::Run() sets this on its own thread
         * - BusinessThread::Run() sets this on its dedicated worker thread
         *
         * 多 Context 正确性由现有测试覆盖。
         */
        inline thread_local IResolveContext* g_current_context = nullptr;

        class ScopedCurrentIpcContext
        {
        public:
            explicit ScopedCurrentIpcContext(IResolveContext* ctx)
            {
                g_current_context = ctx;
            }
            ~ScopedCurrentIpcContext() { g_current_context = nullptr; }
            ScopedCurrentIpcContext(const ScopedCurrentIpcContext&) = delete;
            ScopedCurrentIpcContext& operator=(const ScopedCurrentIpcContext&) =
                delete;
        };

        inline IResolveContext* GetCurrentIpcContext()
        {
            return g_current_context;
        }

    } // namespace IPC
} // namespace Core
DAS_NS_END
