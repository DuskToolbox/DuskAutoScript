#ifndef DAS_CORE_IPC_MANUAL_PROXY_REGISTRY_H
#define DAS_CORE_IPC_MANUAL_PROXY_REGISTRY_H

#include <cstdint>
#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/DasPtr.hpp>
#include <unordered_map>

DAS_CORE_IPC_NS_BEGIN

using ManualProxyFactory =
    IDasBase* (*)(uint32_t                      interface_id,
                  const ObjectId&               object_id,
                  IpcRunLoop&                   run_loop,
                  std::weak_ptr<BusinessThread> business_thread,
                  ProxyFactory&                 proxy_factory);

// Try manual registry first for interfaces that need custom IPC semantics,
// then fall back to the generated proxy factory.
DasPtr<IDasBase> CreateProxyByInterfaceIdWithFallback(
    uint32_t                      interface_id,
    const ObjectId&               object_id,
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    ProxyFactory&                 proxy_factory);

void RegisterManualProxyFactory(
    uint32_t           interface_id,
    ManualProxyFactory factory);

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_MANUAL_PROXY_REGISTRY_H
