/**
 * @file FakeMainProcess.h
 * @brief 假主进程实现 - 用于测试 Host 的父进程监控功能
 *
 * 支持两种模式：
 * 1. 简单模式：只创建 IPC 上下文，不创建管道（测试父进程监控）
 * 2. 完整握手模式：创建管道并完成握手（测试真实握手后杀主进程）
 */

#ifndef DAS_TEST_FAKE_MAIN_PROCESS_H
#define DAS_TEST_FAKE_MAIN_PROCESS_H

#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/sync/named_mutex.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace FakeMainProcess
{

    /**
     * @brief 共享内存数据结构 - 用于传递 host_pid 和握手状态
     *
     * 命名约定: "das_killparent_{main_pid}"
     */
    struct KillParentSharedData
    {
        uint32_t host_pid;        // DasHost 的 PID（由测试框架写入）
        uint32_t host_pid_ready;  // host_pid 已写入标志（1 = 已写入）
        uint32_t handshake_done;  // 握手完成标志（1 = 完成）
        uint32_t reserved;        // 保留对齐
    };

    /**
     * @brief 假主进程就绪信号
     *
     * 同步协议：
     * 1. Test 创建 SignalHolder(signal_name) - 持有锁
     * 2. Test 启动 FakeMain
     * 3. FakeMain 创建 FakeMainReadySignal(signal_name) - 不持有锁
     * 4. FakeMain 调用 AcquireAndRelease() - 等待获取锁，然后释放
     * 5. Test 调用 SignalHolder::ReleaseAndWait() - 释放锁，等待 FakeMain 获取并释放
     */
    class FakeMainReadySignal
    {
    public:
        explicit FakeMainReadySignal(const std::string& signal_name);
        ~FakeMainReadySignal();

        // FakeMain 调用：等待获取锁，然后释放（表示就绪）
        void AcquireAndRelease();

        static void Cleanup(const std::string& signal_name);

    private:
        std::string signal_name_;
        std::unique_ptr<boost::interprocess::named_mutex> mutex_;
    };

    /**
     * @brief 测试端使用的信号持有者
     *
     * 构造时持有锁，ReleaseAndWait() 释放并等待 FakeMain 就绪
     */
    class SignalHolder
    {
    public:
        explicit SignalHolder(const std::string& signal_name);
        ~SignalHolder();

        // 释放锁并等待 FakeMain 获取后释放（表示就绪）
        bool ReleaseAndWait(std::chrono::milliseconds timeout_ms);

    private:
        std::string signal_name_;
        std::unique_ptr<boost::interprocess::named_mutex> mutex_;
        bool holds_lock_;
    };

    /**
     * @brief 共享内存管理器
     *
     * 管理用于传递 host_pid 的共享内存
     */
    class KillParentSharedMemory
    {
    public:
        explicit KillParentSharedMemory(const std::string& shm_name);
        ~KillParentSharedMemory();

        /**
         * @brief 等待 host_pid 被写入
         * @param timeout_ms 超时时间
         * @return host_pid，超时返回 0
         */
        uint32_t WaitForHostPid(std::chrono::milliseconds timeout_ms);

        /**
         * @brief 设置握手完成标志
         */
        void SetHandshakeDone();

        /**
         * @brief 等待共享内存被创建（由测试框架调用）
         * @param shm_name 共享内存名称
         * @param timeout_ms 超时时间
         * @return true 共享内存已创建
         */
        static bool WaitForSharedMemoryReady(
            const std::string& shm_name,
            std::chrono::milliseconds timeout_ms);

        /**
         * @brief 写入 host_pid（由测试框架调用）
         * @param shm_name 共享内存名称
         * @param host_pid DasHost 的 PID
         */
        static void WriteHostPid(const std::string& shm_name, uint32_t host_pid);

        /**
         * @brief 等待握手完成（由测试框架调用）
         * @param shm_name 共享内存名称
         * @param timeout_ms 超时时间
         * @return true 握手完成
         */
        static bool WaitForHandshakeDone(
            const std::string& shm_name,
            std::chrono::milliseconds timeout_ms);

        /**
         * @brief 清理共享内存
         */
        static void Cleanup(const std::string& shm_name);

    private:
        std::string shm_name_;
        boost::interprocess::managed_shared_memory* shm_;
        KillParentSharedData* data_;
        std::unique_ptr<boost::interprocess::named_mutex> mutex_;
    };

    std::string GenerateUniqueSignalName();
    int         RunFakeMainProcessMode(const std::string& signal_name);

    // 固定的共享内存名称（用于 KillParent 测试传递 host_pid）
    constexpr const char* KILL_PARENT_SHM_NAME = "C74D684A-D1F2-49E7-9CC0-428D5CFB02DE";

} // namespace FakeMainProcess

#endif
