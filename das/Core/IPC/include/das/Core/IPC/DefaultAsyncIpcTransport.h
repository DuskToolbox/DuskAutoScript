#ifndef DAS_CORE_IPC_DEFAULT_ASYNC_IPC_TRANSPORT_H
#define DAS_CORE_IPC_DEFAULT_ASYNC_IPC_TRANSPORT_H

// 包含类型别名定义
#include <das/Core/IPC/AsyncIpcTransport.h>

// 平台相关的 Transport include 封装
// Windows 下两个 transport 同时可用：Win32AsyncIpcTransport (Named Pipe)
// 和 UnixAsyncIpcTransport (AF_UNIX, Win10 1803+)。运行时通过 AfUnixAvailable()
// 选择。
#ifdef DAS_WINDOWS
#include <das/Core/IPC/UnixAsyncIpcTransport.h>
#include <das/Core/IPC/Win32AsyncIpcTransport.h>
#else
#include <das/Core/IPC/UnixAsyncIpcTransport.h>
#endif

#endif // DAS_CORE_IPC_DEFAULT_ASYNC_IPC_TRANSPORT_H
