#ifndef DAS_CORE_OCVWRAPPER_DASCVMODULEENTRY_H
#define DAS_CORE_OCVWRAPPER_DASCVMODULEENTRY_H

#include <das/Core/IPC/MainProcess/IIpcContext.h>

DAS_NS_BEGIN
namespace Core
{
    namespace OcvWrapper
    {
        /**
         * @brief DasCore module initialization entry point for OcvWrapper.
         *
         * Called in the main process after IIpcContext creation, registers
         * cv.cpu / cv.cuda services via IIpcContext::RegisterServiceByName().
         *
         * IIpcContext::RegisterServiceByName() internally handles thread
         * scheduling (see IpcContext.cpp:816), so any thread can call this.
         *
         * @param p_ipc_context Main process IPC context pointer (non-null)
         * @return DAS_S_OK on success
         */
        DAS_API DasResult
        InitDasCore(IPC::MainProcess::IIpcContext* p_ipc_context);
    } // namespace OcvWrapper
} // namespace Core
DAS_NS_END

#endif // DAS_CORE_OCVWRAPPER_DASCVMODULEENTRY_H
