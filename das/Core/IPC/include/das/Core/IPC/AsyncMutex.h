#ifndef DAS_CORE_IPC_ASYNC_MUTEX_H
#define DAS_CORE_IPC_ASYNC_MUTEX_H

#include <boost/asio.hpp>
#include <memory>
#include <queue>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

class AsyncMutex
{
    struct State
    {
        bool locked = false;
        bool cancelled = false;
        std::queue<std::shared_ptr<boost::asio::steady_timer>> waiters;
    };

public:
    explicit AsyncMutex(boost::asio::io_context& io_ctx)
        : io_ctx_(io_ctx), state_(std::make_shared<State>())
    {
    }

    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    AsyncMutex(AsyncMutex&&) noexcept = default;
    AsyncMutex& operator=(AsyncMutex&&) noexcept = default;

    boost::asio::awaitable<bool> Lock()
    {
        // Capture shared_ptr in coroutine frame to prevent use-after-free
        // if AsyncMutex is destroyed during co_await suspension.
        // The destructor std::move(state_) out, leaving this->state_ empty.
        auto state = state_;

        if (!state->locked)
        {
            state->locked = true;
            co_return false;
        }

        auto timer = std::make_shared<boost::asio::steady_timer>(io_ctx_.get());
        timer->expires_at(std::chrono::steady_clock::time_point::max());
        state->waiters.push(timer);

        try
        {
            co_await timer->async_wait(boost::asio::use_awaitable);
        }
        catch (const boost::system::system_error&)
        {
            // timer cancelled by Unlock() or destructor
        }

        co_return state->cancelled;
    }

    void Unlock()
    {
        if (state_->waiters.empty())
        {
            state_->locked = false;
            return;
        }
        auto timer = state_->waiters.front();
        state_->waiters.pop();
        timer->cancel();
    }

    ~AsyncMutex()
    {
        if (!io_ctx_.get().stopped())
        {
            boost::asio::post(
                io_ctx_.get(),
                [state = std::move(state_)]()
                {
                    state->cancelled = true;
                    while (!state->waiters.empty())
                    {
                        auto timer = state->waiters.front();
                        state->waiters.pop();
                        timer->cancel();
                    }
                });
        }
    }

    boost::asio::io_context& IoCtx() noexcept { return io_ctx_; }

private:
    std::reference_wrapper<boost::asio::io_context> io_ctx_;
    std::shared_ptr<State>                          state_;
};

DAS_CORE_IPC_NS_END

#endif
