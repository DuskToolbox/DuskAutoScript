#ifndef DAS_CORE_IPC_TEST_TEST_TRANSPORT_RESOLVERS_H
#define DAS_CORE_IPC_TEST_TEST_TRANSPORT_RESOLVERS_H

#include <cstdint>
#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/IHostConnection.h>
#include <das/DasPtr.hpp>

/// main 语义解析体（两级查表）单源：FindManagedHostConnection 命中取
/// host->GetTransport()，否则 FindHostLocalTransport——各测试文件 run
/// loop 构造点共用（模拟 main 角色的发送解析，显式角色声明），与生产
/// MainProcess::IpcContext 注入的 resolver 同义
inline DAS::Core::IPC::TransportLookupResult ResolveTransportMainSemantic(
    DAS::Core::IPC::ConnectionManager& connection_manager,
    uint16_t                           target_session_id)
{
    const DAS::DasPtr<DAS::Core::IPC::IHostConnection> host =
        connection_manager.FindManagedHostConnection(target_session_id);
    if (host)
    {
        return host->GetTransport();
    }
    return connection_manager.FindHostLocalTransport(target_session_id);
}

#endif // DAS_CORE_IPC_TEST_TEST_TRANSPORT_RESOLVERS_H
