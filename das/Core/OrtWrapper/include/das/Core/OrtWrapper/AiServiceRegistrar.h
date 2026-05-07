#ifndef DAS_CORE_ORTWRAPPER_AISERVICEREGISTRAR_H
#define DAS_CORE_ORTWRAPPER_AISERVICEREGISTRAR_H

#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/OrtWrapper/Config.h>

DAS_CORE_ORTWRAPPER_NS_BEGIN

/**
 * @brief Register ai.cpu service for the main process.
 *
 * Uses IIpcContext::RegisterServiceByName (not the C API
 * DasRegisterMainProcessServiceByName), because IIpcContext member methods
 * already handle thread scheduling internally.
 *
 * @param ipc_context Main process IPC context reference
 * @return DAS_S_OK on success
 */
DasResult RegisterAiServices(IPC::MainProcess::IIpcContext& ipc_context);

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_AISERVICEREGISTRAR_H
