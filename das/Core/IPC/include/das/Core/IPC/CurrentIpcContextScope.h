#pragma once

#include <das/DasExport.h>
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
         * @brief Read the per-thread IPC resolve context.
         *
         * Defined in libDasCore's single translation unit (see
         * CurrentIpcContextScope.cpp): the storage must NOT be an inline
         * variable in this header, because an inline thread_local gives
         * every DLL its own copy — a plugin DLL reading it would always see
         * null even after the Host BusinessThread (inside libDasCore) set
         * its own copy. Keep it that way; plugin code must use the C ABI
         * (DasQueryMainProcessInterfaceByName) instead.
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
        DAS_API IResolveContext* GetCurrentIpcContext();

        /**
         * @brief Overwrite the per-thread IPC resolve context (see
         * GetCurrentIpcContext for why this lives outside this header).
         */
        DAS_API void SetCurrentIpcContext(IResolveContext* context);

        class ScopedCurrentIpcContext
        {
        public:
            explicit ScopedCurrentIpcContext(IResolveContext* ctx)
            {
                SetCurrentIpcContext(ctx);
            }
            ~ScopedCurrentIpcContext() { SetCurrentIpcContext(nullptr); }
            ScopedCurrentIpcContext(const ScopedCurrentIpcContext&) = delete;
            ScopedCurrentIpcContext& operator=(const ScopedCurrentIpcContext&) =
                delete;
        };

    } // namespace IPC
} // namespace Core
DAS_NS_END
