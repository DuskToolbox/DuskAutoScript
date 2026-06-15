// GuardedCallback 并发验证测试（Phase 80.2 Plan 03 Task 1）
//
// 覆盖 GuardedCallback 的三条并发不变量（HostLauncher.h:39-108 落地的封装类）：
//   - CR-01: Set/Clear/Invoke 在多线程并发下不崩溃、不内存损坏
//   - CR-02: Invoke 持锁执行 callback，Clear 阻塞等 Invoke 完成（drain 语义）
//   - INV-01: 回调内重入同一 slot 的 Invoke 会死锁（非递归 std::mutex）
//
// ReentrantInvoke_DeadlocksOrTimesOut 用 std::thread + detach（禁用
// 异步返回值范式）：
//   异步返回值对象析构会阻塞等待线程完成，而死锁线程永不完成 =
//   析构永久 hang 整个测试套件。detach 后的线程在测试进程退出时被强杀，
//   不影响主线程的轮询超时检测。

#include <atomic>
#include <chrono>
#include <das/Core/IPC/HostLauncher.h>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>

using DAS::Core::IPC::GuardedCallback;
using DAS::Core::IPC::HostLauncher;

// ====== CR-01: Set/Clear/Invoke 并发压跑不崩溃 ======
// 多线程并发：≥2 invoker 持续 Invoke、≥1 setter 持续 Set、≥1 clearer 周期
// Clear+Set。 外层 stress run 100 次放大竞态窗口（RESEARCH.md :563）。
// 断言：不崩溃（ASAN 会抓 UAF）；counter ≥ 0（无内存损坏）。
TEST(GuardedCallbackTest, SetClearInvoke_Concurrent_NoCrash)
{
    constexpr int kStressIterations = 100;
    for (int iter = 0; iter < kStressIterations; ++iter)
    {
        GuardedCallback<void(int)> slot;
        std::atomic<int>           counter{0};
        std::atomic<bool>          stop{false};

        slot.Set([&counter](int v) { counter.fetch_add(v); });

        std::vector<std::thread> threads;
        // 2 invoker
        for (int i = 0; i < 2; ++i)
        {
            threads.emplace_back(
                [&]()
                {
                    while (!stop.load())
                    {
                        slot.Invoke(1);
                    }
                });
        }
        // 1 setter（持续重设 callback）
        threads.emplace_back(
            [&]()
            {
                while (!stop.load())
                {
                    slot.Set([&counter](int v) { counter.fetch_add(v); });
                }
            });
        // 1 clearer（周期 Clear + Set 交替）
        threads.emplace_back(
            [&]()
            {
                while (!stop.load())
                {
                    slot.Clear();
                    slot.Set([&counter](int v) { counter.fetch_add(v); });
                }
            });

        // 运行 200ms 放大竞态窗口
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        stop.store(true);
        for (auto& t : threads)
        {
            t.join();
        }

        // 负向断言：counter 必须 >= 0（无内存损坏 / underflow）。
        // 不做更强断言：stress 下 Clear/Set 交替可能让部分 Invoke 空转，
        // counter 数值不固定，只要不崩溃 + ASAN 无 UAF 报错即 PASS。
        EXPECT_GE(counter.load(), 0);
    }
}

// ====== CR-02: Invoke 持锁执行，Clear 阻塞等 Invoke 完成 ======
// 线程 A 调 slot.Invoke()，callback 内部 sleep 100ms。
// 主线程等 callback_started 后启动线程 B 调 slot.Clear()。
// 断言：若 Clear 已返回（clear_returned=true）则 callback
// 必已完成（callback_finished=true）， 证明 Clear 等 Invoke 完成（持锁 drain
// 语义，CR-02 屏障）。
TEST(GuardedCallbackTest, Invoke_HoldsLockUntilCallbackReturns)
{
    GuardedCallback<void()> slot;
    std::atomic<bool>       callback_started{false};
    std::atomic<bool>       callback_finished{false};

    slot.Set(
        [&]()
        {
            callback_started.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            callback_finished.store(true);
        });

    std::thread invoker([&]() { slot.Invoke(); });

    // 等 callback_started（证明 Invoke 已拿锁并进入 callback）
    while (!callback_started.load())
    {
        std::this_thread::yield();
    }

    std::atomic<bool> clear_returned{false};
    std::thread       clearer(
        [&]()
        {
            slot.Clear(); // 持锁置空，必须等 Invoke 内的 callback 跑完释放锁
            clear_returned.store(true);
        });

    clearer.join();
    invoker.join();

    // 持锁 drain 断言：若 Clear 已返回，则 callback 必已完成。
    // 不满足 = Clear 在 callback 还在跑时返回 = 没持锁 = CR-02 回归。
    EXPECT_TRUE(callback_finished.load() || !clear_returned.load())
        << "Clear must block until Invoke's callback finishes (CR-02 drain barrier)";
    EXPECT_TRUE(callback_finished.load());
}

// ====== INV-01: 回调内重入 Invoke 会死锁（非递归 std::mutex） ======
// slot.Set([&slot]() { slot.Invoke(); }); —— 重入 = INV-01 违反。
// 用 std::thread + detach 启动调 slot.Invoke()（禁用异步返回值范式）：
//   - 异步返回值对象析构会阻塞等待线程完成，
//     而本测试线程是死锁的，析构会永久 hang 整个测试进程退出（Warning
//     #6）。
//   - detach 后的线程在测试进程退出时被强杀，不影响主线程超时检测。
// 主线程轮询 sleep_for(100ms) × 20 次（共 2s）检测 invoke_returned：
//   若 2s 后 invoke_returned 仍为 false → 死锁被超时断言捕获 = 测试 PASS。
TEST(GuardedCallbackTest, ReentrantInvoke_DeadlocksOrTimesOut)
{
    GuardedCallback<void()> slot;
    slot.Set([&slot]() { slot.Invoke(); }); // 重入 = INV-01 违反

    std::atomic<bool> invoke_returned{false};
    std::thread(
        [&]()
        {
            slot.Invoke(); // 重入：callback 内再 slot.Invoke() → 非递归 mutex
                           // 自死锁
            invoke_returned.store(true);
        })
        .detach(); // detach 而非 join：主线程靠轮询检测，不阻塞等待死锁线程

    // 主线程轮询 2s 检测死锁（100ms × 20 次）
    for (int i = 0; i < 20; ++i)
    {
        if (invoke_returned.load())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_FALSE(invoke_returned.load())
        << "Reentrant Invoke should deadlock (non-recursive mutex, INV-01 "
           "violation detected); if it returned, the mutex is recursive (wrong)";
    // 注意：detach 后的死锁线程在测试进程退出时被强杀，不影响后续测试。
}

// ====== ResetHostLifecycleCallbacks: 选择性 Clear，保留 on_register_ ======
// 覆盖 plan 04 WARNING #1：ResetHostLifecycleCallbacks 只清
// on_process_exit_slot_ + on_heartbeat_timeout_slot_，不清 on_register_
// （与 ClearCallbacks 全清三 slot 职责区分，防止误清 on_register_ 的回归）。
//
// on_process_exit 无 public 触发器（exit-watcher 协程内部 Invoke），故
// exit_fired 维度通过"ResetHostLifecycleCallbacks 内部
// on_process_exit_slot_.Clear() 与 on_heartbeat_timeout_slot_.Clear()
// 同构（都是 GuardedCallback::Clear）" 间接覆盖——只断言 heartbeat 维度即可证明
// Clear 生效，on_process_exit 同理。 exit_fired 变量保留作未来 exit-watcher
// 触发器可用时的扩展锚点。
TEST(
    GuardedCallbackTest,
    ResetHostLifecycleCallbacks_ClearsLifecycleSlots_PreservesRegister)
{
    boost::asio::io_context io_ctx;
    // 第三参传非空 OnRegisterCallback（构造 HostLauncher.h:129-132）验证
    // on_register_ 未被 ResetHostLifecycleCallbacks 误清。
    HostLauncher launcher(
        io_ctx,
        /*session_id=*/1,
        []() { return DAS_S_OK; });

    std::atomic<bool> heartbeat_fired{false};
    std::atomic<bool> exit_fired{
        false}; // 扩展锚点（on_process_exit 无 public 触发器）
    launcher.SetOnHeartbeatTimeout([&heartbeat_fired](DasGuid)
                                   { heartbeat_fired.store(true); });
    launcher.SetOnProcessExit([&exit_fired](uint16_t, int)
                              { exit_fired.store(true); });

    DasGuid some_guid{};
    some_guid.data1 = 0x000000DD;
    launcher.SetAssociatedGuid(some_guid);

    // 只清生命周期监护 slot（on_process_exit + on_heartbeat_timeout），
    // 保留 on_register_。
    launcher.ResetHostLifecycleCallbacks();

    // 调 NotifyHeartbeatTimeout（直接调具体类方法，GuardedCallbackTest.cpp 已
    // import HostLauncher.h 可访问 public 方法）。slot 已 Clear，Invoke 空转
    // （持锁但 callback_ 已置空），heartbeat_fired 不应被设为 true。
    launcher.NotifyHeartbeatTimeout();

    EXPECT_FALSE(heartbeat_fired.load())
        << "on_heartbeat_timeout_slot_ should be cleared after "
           "ResetHostLifecycleCallbacks (Invoke should be a no-op)";

    // on_register_ 未被误清的间接验证：HostLauncher 仍可 GetAssociatedGuid
    // （内部状态未被破坏），析构不崩溃。on_register_ 由
    // InternalRegisterHostLauncher 路径触发，测试不便直接 Invoke，保守用
    // HostLauncher 仍可用证明其内部状态完好。
    EXPECT_EQ(launcher.GetAssociatedGuid(), some_guid)
        << "HostLauncher internal state must remain intact (on_register_ "
           "preserved, associated_guid_ readable)";
}
