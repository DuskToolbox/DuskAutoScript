/*
 * Copyright (c) 2021-2022 NVIDIA Corporation
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <gtest/gtest.h>
#include <iostream>

// Pull in the reference implementation of P2300:
#include <stdexec/execution.hpp>

#include <exec/async_scope.hpp>
#include <exec/static_thread_pool.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <das/Core/IPC/AsioSender.h>
#include <das/DasTypes.hpp>

#include <memory>
#include <thread>

using namespace stdexec;

TEST(StdexecTest, StaticThreadPoolTest)
{
    exec::static_thread_pool ctx{8};
    scheduler auto           sch = ctx.get_scheduler(); // 1
                                                        //
    sender auto begin = schedule(sch);                  // 2
    sender auto hi_again = then(                                           // 3
        begin,                                                               // 3
        [] {                                                                 // 3
            std::cout << "Hello world! Have an int.\n";                        // 3
            return 13;                                                         // 3
        });                                                                  // 3

    sender auto add_42 = then(hi_again, [](int arg) { return arg + 42; }); // 4
    auto [i] = sync_wait(add_42).value();                                  // 5
    std::cout << "Result: " << i << std::endl;

    // Sync_wait provides a run_loop scheduler
    const std::tuple<run_loop::__scheduler> t =
        sync_wait(get_scheduler()).value();
    (void)t;

    auto y = let_value(
        get_scheduler(),
        [](auto sched)
        {
            return starts_on(
                sched,
                then(
                    just(),
                    []
                    {
                        std::cout << "from run_loop\n";
                        return 42;
                    }));
        });
    sync_wait(y);

    sync_wait(when_all(just(42), get_scheduler(), get_stop_token()));
}

// 91-T5: AsioSenderToken 把 co_spawn 的 awaitable 包成 stdexec sender，
// 可被 sync_wait 取回返回值。
TEST(StdexecTest, AsioSenderTokenSmoke)
{
    boost::asio::io_context io_ctx;
    auto                    sender = boost::asio::co_spawn(
        io_ctx,
        []() -> boost::asio::awaitable<int> { co_return 42; },
        DAS::Core::IPC::AsioSenderToken{});

    // io_context 必须跑在另一线程，主线程 sync_wait 阻塞等 completion。
    std::thread t(
        [&]
        {
            io_ctx.run();
        });
    auto [v] = stdexec::sync_wait(std::move(sender)).value();
    io_ctx.stop();
    t.join();

    EXPECT_EQ(v, 42);
}

// 91-T6: 带值/可失败 sender 经 let_error(->just_stopped()) + then 适配成
// set_value()/set_stopped() 后可被 async_scope::spawn 接受，on_empty() 完成。
TEST(StdexecTest, AsyncScopeSpawnAdaptation)
{
    exec::async_scope scope;

    auto adapted = stdexec::just(DAS_S_OK)
        | stdexec::let_error(
            [](std::exception_ptr /* e */)
            {
                std::cout << "unexpected error in AsyncScopeSpawnAdaptation\n";
                return stdexec::just_stopped();
            })
        | stdexec::then([](DasResult /* r */) {});

    scope.spawn(std::move(adapted));
    stdexec::sync_wait(scope.on_empty());
}

// 91-T7: awaitable 抛异常 -> 桥接层转成 set_error(std::exception_ptr)，
// sync_wait 会 rethrow；测试体不崩溃、不 terminate。
TEST(StdexecTest, AsioSenderTokenErrorPropagation)
{
    boost::asio::io_context io_ctx;
    auto                    sender = boost::asio::co_spawn(
        io_ctx,
        []() -> boost::asio::awaitable<int>
        {
            throw std::runtime_error("boom");
            co_return 42;
        },
        DAS::Core::IPC::AsioSenderToken{});

    std::thread t(
        [&]
        {
            io_ctx.run();
        });

    EXPECT_THROW(stdexec::sync_wait(std::move(sender)), std::runtime_error);

    io_ctx.stop();
    t.join();
}

// 91-T8: operation_aborted -> set_stopped（而非 set_error）。
// 取消与错误必须区分，否则 Stop() 的 request_stop->terminal 取消会被当错误传播。
TEST(StdexecTest, AsioSenderTokenCancellationAsStopped)
{
    boost::asio::io_context io_ctx;
    auto                    timer = std::make_shared<boost::asio::steady_timer>(
        io_ctx,
        std::chrono::seconds(30));

    auto sender = boost::asio::co_spawn(
        io_ctx,
        [timer]() -> boost::asio::awaitable<int>
        {
            co_await timer->async_wait(boost::asio::use_awaitable);
            co_return 42;
        },
        DAS::Core::IPC::AsioSenderToken{});

    std::thread io_thread(
        [&]
        {
            io_ctx.run();
        });

    // 后台线程取消 timer，产生 operation_aborted。循环直到真正取消到 pending wait，
    // 避免与协程启动 async_wait 的时序竞争。
    std::thread canceller(
        [&]
        {
            while (timer->cancel() == 0)
            {
                std::this_thread::yield();
            }
        });

    auto result = stdexec::sync_wait(std::move(sender));
    canceller.join();

    // set_stopped -> sync_wait 返回 nullopt（stopped 形态），而非 error。
    EXPECT_FALSE(result.has_value());

    io_ctx.stop();
    io_thread.join();
}
