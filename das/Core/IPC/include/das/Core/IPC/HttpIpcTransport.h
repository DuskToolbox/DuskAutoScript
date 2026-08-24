#ifndef DAS_CORE_IPC_HTTP_IPC_TRANSPORT_H
#define DAS_CORE_IPC_HTTP_IPC_TRANSPORT_H

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <das/Core/IPC/Config.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BEAST_WARNING
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
DAS_DISABLE_WARNING_END

#include <cstddef>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <memory>
#include <string>
#include <variant>
#include <vector>

DAS_CORE_IPC_NS_BEGIN

class SharedMemoryPool;

/**
 * @brief 基于 WebSocket 的 IPC 传输实现
 * @details 使用 WebSocket 二进制帧替代 Named Pipe 进行 IPC 通信，
 *          实现与 Win32AsyncIpcTransport 相同的消息语义。
 *
 * 消息格式: 单个 WebSocket 二进制帧 = [IPCMessageHeader(32B)][body(body_size)]
 *
 * 与 Named Pipe 版本的区别:
 * - 使用 AsyncMutex 保证并发写入安全（boost::beast::websocket::stream
 *   不允许并发 async_write）
 * - 无需 SharedMemoryPool（大消息直接作为完整帧发送）
 * - 无平台特定依赖（不依赖 Windows.h 或 unistd.h）
 */
class HttpIpcTransport
{
public:
    /**
     * @brief 从已建立的 TCP 连接构造 WebSocket 传输
     * @param socket 已连接的 TCP socket，所有权转移
     */
    explicit HttpIpcTransport(boost::asio::ip::tcp::socket&& socket);

    /**
     * @brief 从已升级的 WebSocket stream 构造传输
     * @param ws 已完成 WebSocket 升级的 stream，所有权转移
     *
     * 用于 Server 端：HttpIpcServer 完成 WebSocket accept 后，
     * 将已升级的 stream 直接移交。
     */
    explicit HttpIpcTransport(
        boost::beast::websocket::stream<boost::asio::ip::tcp::socket>&& ws);

    ~HttpIpcTransport();

    HttpIpcTransport(const HttpIpcTransport&) = delete;
    HttpIpcTransport& operator=(const HttpIpcTransport&) = delete;

    HttpIpcTransport(HttpIpcTransport&&) noexcept;
    HttpIpcTransport& operator=(HttpIpcTransport&&) noexcept;

    /**
     * @brief 异步发送 IPC 消息
     * @param header 已验证的消息头（32 字节）
     * @param body 消息体数据指针
     * @param body_size 消息体长度
     * @return awaitable<DasResult> DAS_S_OK 成功，DAS_E_IPC_SEND_FAILED 失败
     */
    [[nodiscard]]
    boost::asio::awaitable<DasResult> SendCoroutine(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

    /**
     * @brief scatter-gather 发送重载：合并拷贝为连续 buffer 后单次发送
     * @param header 已验证的外层消息头（32 字节；与段所指备存储同须
     *               存活至本协程完成）
     * @param segments 非拥有段序列（rvalue——段列表 move 进协程帧持有；
     *                 契约见 AsyncIpcTransport.h 的 SendSegment 定义）
     * @note 一条 WebSocket 消息 = 一个 IPC 帧：实现内 ws async_write 恰
     *       出现一次（拆多次 = 中间帧无外层 header 语义、接收端静默
     *       丢尾）；本重载只承接“单外层帧搭车形态”——入口 debug 断言
     *       校验外层 header 的 RESERVED_AS_SIZE 语义位与段数上限，
     *       “N 独立帧合一写”的批量合写形态由 CanBatchFrames() == false
     *       在调用侧分流，永不进入本函数
     */
    [[nodiscard]]
    boost::asio::awaitable<DasResult> SendScatter(
        const ValidatedIPCMessageHeader& header,
        std::vector<SendSegment>&&       segments);

    /**
     * @brief SendScatter 入口形态断言的段数上限（搭车形态：扩展段 + 宿主
     *        body，不含批量合写——该形态被 CanBatchFrames 分流）
     */
    static constexpr size_t MAX_SCATTER_SEGMENTS = 64;

    /**
     * @brief 本传输是否允许“N 独立帧合一写”的批量合写形态（静态能力常量）
     * @details 一条 WebSocket 消息 = 一个 IPC 帧，背靠背多帧尾部会被
     *          接收端静默丢弃——HTTP/WS 恒为 false（批量合写仅
     *          pipe/AF_UNIX 字节流合法）
     */
    static constexpr bool CAN_BATCH_FRAMES = false;

    /**
     * @brief 异步接收 IPC 消息
     * @return awaitable<variant<DasResult, AsyncIpcMessage>>
     *         成功返回 AsyncIpcMessage，失败返回 DasResult 错误码
     */
    [[nodiscard]]
    boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
    ReceiveCoroutine();

    /**
     * @brief 检查连接是否仍然活跃
     */
    [[nodiscard]]
    bool IsConnected() const;

    /**
     * @brief 获取关联的 io_context
     */
    boost::asio::io_context& GetIoContext();

    /**
     * @brief 获取远端端点名称
     */
    [[nodiscard]]
    std::string GetEndpointName() const;

    /**
     * @brief 设置共享内存池（HTTP 模式 no-op）
     * @note HTTP transport 不使用共享内存（大消息直接走完整 WebSocket 帧）。
     *       保留此方法仅为让 AnyTransport::SetSharedMemoryPool 的 std::visit
     *       对所有 transport 类型可编译；pool 注入的实际路径是
     *       InitializeAsync。deprecated，待 IpcContext 调用点迁移后统一移除。
     */
    void SetSharedMemoryPool(SharedMemoryPool* pool);

    /**
     * @brief 关闭连接并释放资源
     */
    void Cleanup();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_HTTP_IPC_TRANSPORT_H
