#ifndef DAS_CORE_IPC_BUSINESS_CONTROL_REQUEST_RAW_H
#define DAS_CORE_IPC_BUSINESS_CONTROL_REQUEST_RAW_H

#include <cstddef>
#include <cstdint>
#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <memory>
#include <vector>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief Send a business-control IPC request and synchronously wait for
 * response
 *
 * Standalone helper that extracts the reusable send pattern from
 * IPCProxyBase::SendBusinessControlRequest. Callers provide the raw
 * IpcRunLoop / BusinessThread references and session IDs so this
 * function works outside of any IPCProxyBase instance.
 *
 * Two-path strategy:
 * - BusinessThread is the calling thread  -> PostSend + PumpUntilResponse
 * - External thread                       -> AwaitResponseSender::start() 中
 *                                           先注册 pending completion 再 PostSend
 *
 * @param run_loop            IPC run loop (call_id allocation, PostSend)
 * @param business_thread     Weak ptr to the BusinessThread (PumpUntilResponse)
 * @param source_session_id   Local session ID (header source)
 * @param target_session_id   Remote session ID (header target)
 * @param command             Business-control command type
 * @param body                Request body bytes (may be nullptr when body_size
 *                            is 0)
 * @param body_size           Size of the request body in bytes
 * @param out_response        [out] Response body
 * @return DasResult DAS_S_OK on success, error code on failure
 */
DasResult SendBusinessControlRequestRaw(
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    uint16_t                      source_session_id,
    uint16_t                      target_session_id,
    IpcCommandType                command,
    const uint8_t*                body,
    size_t                        body_size,
    std::vector<uint8_t>&         out_response);

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_BUSINESS_CONTROL_REQUEST_RAW_H
