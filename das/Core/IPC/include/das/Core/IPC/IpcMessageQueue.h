#ifndef DAS_CORE_IPC_IPC_MESSAGE_QUEUE_H
#define DAS_CORE_IPC_IPC_MESSAGE_QUEUE_H

#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>

#include <boost/circular_buffer.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

DAS_CORE_IPC_NS_BEGIN

/// @brief 入站消息结构体 - 用于 IO 线程到业务线程的消息传递
/// @note 不包含 transport 指针——业务线程不应接触 transport
///       业务线程发送通过 IpcRunLoop::PostSend(header, body) 投递到 IO 线程
struct InboundMessage
{
    /// 默认构造函数
    InboundMessage() = default;

    /// 移动构造函数
    InboundMessage(InboundMessage&&) = default;

    /// 移动赋值运算符
    InboundMessage& operator=(InboundMessage&&) = default;

    /// 删除拷贝构造和赋值
    InboundMessage(const InboundMessage&) = delete;
    InboundMessage& operator=(const InboundMessage&) = delete;

    ValidatedIPCMessageHeader header;
    std::vector<uint8_t>      body;
};

/// @brief 线程安全的固定容量环形缓冲区队列
/// @tparam T 队列中存储的元素类型
template <typename T>
class IpcMessageQueue
{
public:
    /// @brief 构造函数
    /// @param max_elements 队列最大容量
    explicit IpcMessageQueue(size_t max_elements) : ring_(max_elements) {}

    // 禁用拷贝
    IpcMessageQueue(const IpcMessageQueue&) = delete;
    IpcMessageQueue& operator=(const IpcMessageQueue&) = delete;

    // 允许移动
    IpcMessageQueue(IpcMessageQueue&&) = default;
    IpcMessageQueue& operator=(IpcMessageQueue&&) = default;

    /// @brief 入队（非阻塞）
    /// @param msg 要入队的元素（按值传递，内部使用 std::move）
    /// @return 成功返回 DAS_S_OK，队列满返回 DAS_E_IPC_QUEUE_FULL，已关闭返回
    /// DAS_E_IPC_CANCELED
    DasResult Push(T msg)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (uninitialized_)
        {
            return DAS_E_IPC_CANCELED;
        }

        if (ring_.full())
        {
            return DAS_E_IPC_QUEUE_FULL;
        }

        ring_.push_back(std::move(msg));
        cv_.notify_one();

        return DAS_S_OK;
    }

    /// @brief 出队（阻塞）
    /// @return 有元素返回元素，否则返回 nullopt（队列已关闭时）
    std::optional<T> Pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(lock, [this]() { return !ring_.empty() || uninitialized_; });

        if (uninitialized_ && ring_.empty())
        {
            return std::nullopt;
        }

        T item = std::move(ring_.front());
        ring_.pop_front();

        return item;
    }

    /// @brief 出队（阻塞），也可被外部轻量 predicate 唤醒
    /// @param is_done 外部完成条件；必须是轻量/atomic predicate
    /// @return 有元素返回元素；队列关闭或 predicate 完成且无元素时返回 nullopt
    template <typename Predicate>
    std::optional<T> PopUntil(Predicate&& is_done)
    {
        static_assert(
            std::is_invocable_r_v<bool, Predicate&>,
            "IpcMessageQueue::PopUntil predicate must return bool");

        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(
            lock,
            [this, &is_done]()
            { return !ring_.empty() || uninitialized_ || is_done(); });

        if (ring_.empty())
        {
            return std::nullopt;
        }

        T item = std::move(ring_.front());
        ring_.pop_front();

        return item;
    }

    /// @brief 尝试出队（非阻塞）
    /// @return 有元素返回元素，否则返回 nullopt
    std::optional<T> TryPop()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (ring_.empty())
        {
            return std::nullopt;
        }

        T item = std::move(ring_.front());
        ring_.pop_front();

        return item;
    }

    /// @brief 反初始化队列
    /// @note 反初始化后，Push 返回 DAS_E_IPC_CANCELED，Pop 返回 nullopt
    void Uninitialize()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uninitialized_ = true;
        cv_.notify_all();
    }

    /// @brief 唤醒等待者重新检查队列状态和外部 predicate
    void NotifyWaiters() { cv_.notify_all(); }

    /// @brief 检查队列是否已反初始化
    /// @return 已反初始化返回 true
    bool IsUninitialized() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return uninitialized_;
    }

private:
    boost::circular_buffer<T> ring_;
    mutable std::mutex        mutex_;
    std::condition_variable   cv_;
    bool                      uninitialized_ = false;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_MESSAGE_QUEUE_H
