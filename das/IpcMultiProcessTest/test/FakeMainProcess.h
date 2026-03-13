/**
 * @file FakeMainProcess.h
 * @brief 跨进程就绪信号和假主进程实现
 *
 * 使用 boost::interprocess::named_mutex 实现假主进程通知测试主进程"已就绪"。
 * 假主进程在创建 IPC 上下文并准备好接收连接后，调用 NotifyReady()。
 * 测试主进程通过 WaitForReady() 等待就绪信号。
 */

#ifndef DAS_TEST_FAKE_MAIN_PROCESS_H
#define DAS_TEST_FAKE_MAIN_PROCESS_H

#include <boost/interprocess/sync/named_mutex.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace FakeMainProcess {

/**
 * @brief 跨进程就绪信号
 *
 * 使用 boost::interprocess::named_mutex 实现假主进程通知测试主进程"已就绪"。
 * 假主进程在创建 IPC 上下文并准备好接收连接后，调用 NotifyReady()。
 * 测试主进程通过 WaitForReady() 等待就绪信号。
 */
class FakeMainReadySignal {
public:
    explicit FakeMainReadySignal(const std::string& signal_name);
    ~FakeMainReadySignal();

    void NotifyReady();
    bool WaitForReady(std::chrono::milliseconds timeout_ms);
    static void Cleanup(const std::string& signal_name);

private:
    std::string signal_name_;
    std::unique_ptr<boost::interprocess::named_mutex> mutex_;
    bool ready_ = false;
};

std::string GenerateUniqueSignalName();
int RunFakeMainProcessMode(const std::string& signal_name);

} // namespace FakeMainProcess

#endif
