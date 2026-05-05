#include "CvServiceRegistrar.h"
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/Logger/Logger.h>

DAS_NS_BEGIN
namespace Core
{
    namespace OcvWrapper
    {
        /**
         * @brief DasCore module initialization entry point.
         *
         * Called in the main process during startup, after IIpcContext is
         * fully created. Registers cv.cpu / cv.cuda services.
         *
         * IIpcContext::RegisterServiceByName() internally handles thread
         * scheduling (see IpcContext.cpp:816), so any thread can call this.
         *
         * @param p_ipc_context Main process IPC context pointer
         * @return DAS_S_OK on success
         */
        DasResult InitDasCore(IPC::MainProcess::IIpcContext* p_ipc_context)
        {
            if (!p_ipc_context)
            {
                DAS_CORE_LOG_ERROR("InitDasCore: p_ipc_context is null");
                return DAS_E_INVALID_ARGUMENT;
            }

            return RegisterCvServices(*p_ipc_context);
        }
    } // namespace OcvWrapper
} // namespace Core
DAS_NS_END
