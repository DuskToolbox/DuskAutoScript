#ifndef DAS_CORE_OCVWRAPPER_CVSERVICEREGISTRAR_H
#define DAS_CORE_OCVWRAPPER_CVSERVICEREGISTRAR_H

#include "Config.h"
#include <das/Core/IPC/MainProcess/IIpcContext.h>

DAS_CORE_OCVWRAPPER_NS_BEGIN

/**
 * @brief Probe available backends and register cv.cpu / cv.cuda services.
 *
 * per D-01: No new DasCreateCv C API
 * per D-02: Only expose explicit service names cv.cpu / cv.cuda
 * per D-03: One-shot probe at startup, unavailable backends not registered
 * per D-04: Each backend is a global singleton
 *
 * Uses IIpcContext::RegisterServiceByName (not the C API
 * DasRegisterMainProcessServiceByName), because IIpcContext member methods
 * already handle thread scheduling internally (see IpcContext.cpp:816).
 *
 * @param ipc_context Main process IPC context reference
 * @return DAS_S_OK on success (partial failure is logged, not propagated)
 */
DasResult RegisterCvServices(IPC::MainProcess::IIpcContext& ipc_context);

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_CVSERVICEREGISTRAR_H
