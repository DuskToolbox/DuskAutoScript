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
            virtual DasResult RegisterService(IDasBase* p_object, const DasGuid& iid) = 0;
            virtual DasResult UnregisterService(const DasGuid& iid) = 0;

        protected:
            ~IResolveContext() = default;
        };

        // Single TLS pointer 鈥?zero allocation, no vector, no <vector> needed
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
