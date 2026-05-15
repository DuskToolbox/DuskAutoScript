#ifndef DAS_CORE_IPC_AF_UNIX_AVAILABLE_H
#define DAS_CORE_IPC_AF_UNIX_AVAILABLE_H

#include <das/Core/IPC/Config.h>
#include <das/DasConfig.h>

#ifdef DAS_WINDOWS
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/system/error_code.hpp>
#endif

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief 运行时检测 AF_UNIX (Unix Domain Socket) 是否可用
 * @details
 * Meyers 单例 — 测试一次，永久缓存。
 * Windows 上需要 Win10 1803+ 才支持 AF_UNIX；
 * Linux/macOS 始终返回 true。
 */
inline bool AfUnixAvailable()
{
    static const bool available = []
    {
#ifdef DAS_WINDOWS
        // 尝试在 Windows 上创建 Unix Domain Socket（需要 Win10 1803+）
        try
        {
            boost::asio::io_context                     io;
            boost::system::error_code                   ec;
            boost::asio::local::stream_protocol::socket test_socket(io);
            test_socket.open(boost::asio::local::stream_protocol(), ec);
            return !ec;
        }
        catch (...)
        {
            return false;
        }
#else
        return true;
#endif
    }();
    return available;
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_AF_UNIX_AVAILABLE_H
