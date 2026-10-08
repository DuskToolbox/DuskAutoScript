/**
 * @file LoopbackPort.h
 * @brief 回环端口探测共享实现（测试基建，与 IpcTestConfig.h 同家）
 *
 * 单份原件（D-20）：需要监听端口的测试一律 include 本头，禁止复制
 * 副本——双副本只修一份的撞口事故与目录白名单双胞胎漂移同型。
 */

#ifndef DAS_TEST_LOOPBACK_PORT_H
#define DAS_TEST_LOOPBACK_PORT_H

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>

#ifdef DAS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace LoopbackPort
{
    namespace detail
    {
        /// 进程唯一量的跨平台取法（与测试进程内既有 PID 宏先例同构）
        inline std::uint32_t CurrentProcessIdForProbe()
        {
#ifdef DAS_WINDOWS
            return ::GetCurrentProcessId();
#else
            return static_cast<std::uint32_t>(::getpid());
#endif
        }
    } // namespace detail

    /**
     * @brief 探测一个空闲的回环监听端口（bind 探测成功后立即关闭）
     *
     * 返回 0 表示探测失败（socket open 失败或槽内端口全被占用）。
     * 真正的监听发生在调用方之后的配置里——probe-后-close 与监听之间
     * 存在第三方抢占窗口（TOCTOU），本函数无法绝对消除，只能压低概率。
     */
    inline std::uint16_t FindFreeLoopbackPort()
    {
        // 高位固定端口探测带：避开 Windows 动态端口排除范围（当前
        // 49806-53273 被排除）与 well-known 保留端口（<1024）
        constexpr std::uint16_t base_port = 48080;
        constexpr std::uint16_t end_port = 48999;
        constexpr int           max_attempts = 50;

        // 起点掺进程唯一量：两个并发探测进程从同一起点扫同一窗口，会在
        // probe-后-close 的窗口里先后探测到同一「空闲」端口再各自监听
        // （必然撞口）。把探测带切成与探测上限等宽的互不相交槽、以 PID
        // 选槽，碰撞从「必然」降到「同槽小概率」；绝对消除在此探测模型
        // 下不可得，由多轮全绿验收兜底。PID 先除 4 再取模：Windows PID
        // 按 4 对齐，直接取模会令实际可用槽数减半。
        constexpr int slot_count = (end_port - base_port + 1) / max_attempts;
        const std::uint32_t process_id = detail::CurrentProcessIdForProbe();
        const int slot_index = static_cast<int>((process_id / 4u) % slot_count);
        const std::uint16_t start_port =
            static_cast<std::uint16_t>(base_port + slot_index * max_attempts);

        for (int i = 0; i < max_attempts; ++i)
        {
            const std::uint16_t port =
                static_cast<std::uint16_t>(start_port + i);
            if (port > end_port)
            {
                break;
            }
            boost::asio::io_context        io_context;
            boost::asio::ip::tcp::acceptor acceptor(io_context);
            boost::system::error_code      ec;
            acceptor.open(boost::asio::ip::tcp::v4(), ec);
            if (ec)
            {
                return 0;
            }
            acceptor.bind(
                {boost::asio::ip::make_address("127.0.0.1"), port},
                ec);
            if (!ec)
            {
                return port;
            }
        }
        return 0;
    }
} // namespace LoopbackPort

#endif // DAS_TEST_LOOPBACK_PORT_H
