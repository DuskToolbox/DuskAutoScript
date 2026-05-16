#ifndef DAS_CORE_IPC_ANY_TRANSPORT_H
#define DAS_CORE_IPC_ANY_TRANSPORT_H

#include <das/Core/IPC/DefaultAsyncIpcTransport.h>
#include <das/Core/IPC/HttpIpcTransport.h>
#include <das/Core/IPC/UnixAsyncIpcTransport.h>

#include <boost/asio/awaitable.hpp>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

class SharedMemoryPool;

/**
 * @brief 通用 IPC 传输器 — variant 直接存值，零指针跳转
 *
 * Windows: variant<Named Pipe, Unix Socket, HTTP>
 *   AF_UNIX 通过 Meyers singleton 运行时检测可用性
 * Linux:   variant<Unix Socket, HTTP>
 */
class AnyTransport final
{
public:
#ifdef DAS_WINDOWS
    using VariantType = std::variant<
        Win32AsyncIpcTransport,
        UnixAsyncIpcTransport,
        HttpIpcTransport>;
#else
    using VariantType = std::variant<UnixAsyncIpcTransport, HttpIpcTransport>;
#endif

    explicit AnyTransport(Win32AsyncIpcTransport&& t);
    explicit AnyTransport(UnixAsyncIpcTransport&& t);
    explicit AnyTransport(HttpIpcTransport&& t);

    ~AnyTransport();
    AnyTransport(AnyTransport&&) noexcept;
    AnyTransport& operator=(AnyTransport&&) noexcept;
    AnyTransport(const AnyTransport&) = delete;
    AnyTransport& operator=(const AnyTransport&) = delete;

    [[nodiscard]]
    bool                     IsConnected() const;
    boost::asio::io_context& GetIoContext() DAS_LIFETIMEBOUND;
    void                     SetSharedMemoryPool(SharedMemoryPool* pool);
    [[nodiscard]]
    std::string GetEndpointName() const;

    boost::asio::awaitable<DasResult> SendCoroutine(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);
    boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
    ReceiveCoroutine();

    /**
     * @brief 类型安全分发 — std::visit 编译期内联
     *
     * @param pipe_fn  Named Pipe 回调
     * @param unix_fn  Unix Socket 回调
     * @param http_fn  HTTP/WebSocket 回调
     */
    template <typename PipeFn, typename UnixFn, typename HttpFn>
    auto Visit(PipeFn&& pipe_fn, UnixFn&& unix_fn, HttpFn&& http_fn) const
    {
        return std::visit(
            [&](auto& t)
            {
                using T = std::decay_t<decltype(t)>;
#ifdef DAS_WINDOWS
                if constexpr (std::is_same_v<T, Win32AsyncIpcTransport>)
                {
                    return pipe_fn(t);
                }
                else
#endif
                    if constexpr (std::is_same_v<T, UnixAsyncIpcTransport>)
                {
                    return unix_fn(t);
                }
                else
                {
                    return http_fn(t);
                }
            },
            transport_);
    }

    template <typename PipeFn, typename UnixFn, typename HttpFn>
    auto Visit(PipeFn&& pipe_fn, UnixFn&& unix_fn, HttpFn&& http_fn)
    {
        return std::visit(
            [&](auto& t)
            {
                using T = std::decay_t<decltype(t)>;
#ifdef DAS_WINDOWS
                if constexpr (std::is_same_v<T, Win32AsyncIpcTransport>)
                {
                    return pipe_fn(t);
                }
                else
#endif
                    if constexpr (std::is_same_v<T, UnixAsyncIpcTransport>)
                {
                    return unix_fn(t);
                }
                else
                {
                    return http_fn(t);
                }
            },
            transport_);
    }

    [[nodiscard]]
    const VariantType& variant() const noexcept
    {
        return transport_;
    }

    [[nodiscard]]
    VariantType& variant() noexcept
    {
        return transport_;
    }

private:
    VariantType transport_;
};

DAS_CORE_IPC_NS_END

#endif
