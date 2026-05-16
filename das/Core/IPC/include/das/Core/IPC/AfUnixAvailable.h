#ifndef DAS_CORE_IPC_AF_UNIX_AVAILABLE_H
#define DAS_CORE_IPC_AF_UNIX_AVAILABLE_H

#include <das/Core/IPC/Config.h>
#include <das/DasConfig.h>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief 运行时检测 Windows 是否支持 AF_UNIX（Unix Domain Socket）
 * @details
 * Meyers 单例 — 调用 socket(AF_UNIX, SOCK_STREAM, 0) 检测一次，永久缓存。
 * Windows 10 1803+ 原生支持 AF_UNIX；Linux/macOS 始终返回 true。
 */
inline bool AfUnixAvailable()
{
#ifdef DAS_WINDOWS
    static const bool available = []
    {
        const auto sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET)
        {
            return false;
        }
        ::closesocket(sock);
        return true;
    }();
    return available;
#else
    return true;
#endif
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_AF_UNIX_AVAILABLE_H
