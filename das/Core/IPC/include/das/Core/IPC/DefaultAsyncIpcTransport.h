#ifndef DAS_CORE_IPC_DEFAULT_ASYNC_IPC_TRANSPORT_H
#define DAS_CORE_IPC_DEFAULT_ASYNC_IPC_TRANSPORT_H

// 包含类型别名定义
#include <das/Core/IPC/AsyncIpcTransport.h>

// 平台相关的 Transport include 封装
#ifdef _WIN32
#include <das/Core/IPC/Win32AsyncIpcTransport.h>
#else
#include <das/Core/IPC/UnixAsyncIpcTransport.h>
#endif

#endif // DAS_CORE_IPC_DEFAULT_ASYNC_IPC_TRANSPORT_H
