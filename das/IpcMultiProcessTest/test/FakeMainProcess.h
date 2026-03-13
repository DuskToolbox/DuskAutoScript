/**
 * @file FakeMainProcess.h
 * @brief 跨进程就绪信号和假主进程实现
 *
 * 使用 boost::interprocess::named_mutex 实现跨进程同步。
 * 同步机制：
 * 1. 测试主进程先获取并持有锁
 * 2. FakeMain 尝试获取锁（会阻塞等待）
 * 3. 测试主进程释放锁，FakeMain 获取到锁并执行初始化
 * 4. FakeMain 释放锁并通知（通过再次获取锁来标记完成）
 * 5. 测试主进程再次尝试获取锁，如果成功则说明 FakeMain 已就绪
 */

#ifndef DAS_TEST_FAKE_MAIN_PROCESS_H
#define DAS_TEST_FAKE_MAIN_PROCESS_H

#include <boost/interprocess/sync/named_mutex.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace FakeMainProcess
{

    /**
     * @brief 跨进程就绪信号
     *
     * 使用 boost::interprocess::named_mutex 实现跨进程同步。
     * 同步机制确保 FakeMain 在测试主进程检测到就绪之前完成初始化。
     */
    class FakeMainReadySignal
    {
    public:
        explicit FakeMainReadySignal(const std::string& signal_name);
        ~FakeMainReadySignal();

        void        NotifyReady();
        bool        WaitForReady(std::chrono::milliseconds timeout_ms);
        static void Cleanup(const std::string& signal_name);

    private:
        std::string                                       signal_name_;
        std::unique_ptr<boost::interprocess::named_mutex> mutex_;
    };

    std::string GenerateUniqueSignalName();
    int         RunFakeMainProcessMode(const std::string& signal_name);

} // namespace FakeMainProcess

#endif
