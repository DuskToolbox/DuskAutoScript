#ifndef DAS_CORE_IPC_ASIO_SENDER_H
#define DAS_CORE_IPC_ASIO_SENDER_H

/**
 * @file AsioSender.h
 * @brief awaitable -> stdexec sender 桥（AsioSenderToken + AsioSender）
 *
 * 把整个 boost::asio 协程（co_spawn）包成 stdexec sender，供项目标准
 * sender/receiver 编排层消费（D-91-03 方案 A / D-91-05）。镜像 vendored
 * asioexec/completion_token.hpp 的 async_result 机制，但不依赖 asioexec /
 * STDEXEC_ENABLE_ASIO（D-91-04）。
 *
 * 用法：
 * @code
 * auto sender = boost::asio::co_spawn(
 *     io_ctx, awaitable_fn, DAS::Core::IPC::AsioSenderToken{});
 * auto [v] = stdexec::sync_wait(std::move(sender)).value();
 * @endcode
 *
 * 取消语义：receiver env 的 inplace_stop_token 经 stop_callback 桥接到
 * asio cancellation_signal，emit cancellation_type::terminal（D-91-05）。
 * awaitable 抛异常 -> set_error(std::exception_ptr)；operation_aborted ->
 * set_stopped（91-T7 / 91-T8，取消与错误必须区分）。
 */

#include <das/Core/IPC/Config.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_STDEXEC_PARAMETERS

#include <stdexec/execution.hpp>

DAS_DISABLE_WARNING_END

#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>

#include <exception>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

DAS_CORE_IPC_NS_BEGIN

//=============================================================================
// AsioSenderToken — boost::asio completion token 标记类型
//=============================================================================

struct AsioSenderToken
{
};

namespace detail
{
    //=============================================================================
    // completion_signatures 组装（镜像 completion_token.hpp:97-110，但剥离
    // co_spawn 签名的首参 std::exception_ptr）
    //=============================================================================

    // co_spawn 的 asio 签名是 void(std::exception_ptr, T...)，把首参剥离：
    // void(std::exception_ptr, T...) -> stdexec::set_value_t(T...)
    template <typename Signature>
    struct signature;

    template <typename... Args>
    struct signature<void(std::exception_ptr, Args...)>
    {
        using type = stdexec::set_value_t(Args...);
    };

    // 无参完成签名（asio::post 一类操作经 AsioSenderToken 实例化）没有
    // 可剥离的首参，直接映射 set_value 空值域。用全特化而非泛化偏特化：
    // 其余未预期签名保持主模板未定义的硬失败，不会被静默发明成值语义。
    template <>
    struct signature<void()>
    {
        using type = stdexec::set_value_t();
    };

    template <typename... Signatures>
    using completion_signatures = stdexec::completion_signatures<
        typename signature<Signatures>::type...,
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    //=============================================================================
    // set_value 转发（镜像 completion_token.hpp:49-95）：把 asio 实际发送的值
    // 转换成与声明的 completion signature 匹配的类型后再 set_value
    //=============================================================================

    template <typename>
    struct has_function_call_operator
    {
        struct type
        {
        };

        std::tuple<> operator()(type) const;
    };

    template <typename... Args>
    struct has_function_call_operator<stdexec::set_value_t(Args...)>
    {
        std::tuple<Args...> operator()(Args...) const;
    };

    template <typename>
    struct overload_set;

    template <typename... Signatures>
    struct overload_set<stdexec::completion_signatures<Signatures...>>
        : has_function_call_operator<Signatures>...
    {
        using has_function_call_operator<Signatures>::operator()...;
    };

    template <typename T, typename U>
        requires std::is_same_v<T&&, U&&> || std::is_convertible_v<U&&, T&&>
    constexpr T&& convert(U&& u) noexcept
    {
        return static_cast<U&&>(u);
    }

    template <typename T, typename U>
    constexpr std::remove_cvref_t<T> convert(U&& u)
    {
        return static_cast<U&&>(u);
    }

    template <
        typename Tuple,
        typename Receiver,
        std::size_t... Ns,
        typename... Args>
    constexpr void
    set_value_impl(Receiver&& r, std::index_sequence<Ns...>, Args&&... args)
    {
        stdexec::set_value(
            static_cast<Receiver&&>(r),
            convert<std::tuple_element_t<Ns, Tuple>>(
                static_cast<Args&&>(args))...);
    }

    template <typename Signatures, typename Receiver, typename... Args>
    constexpr void set_value(Receiver&& r, Args&&... args)
    {
        using tuple = decltype(std::declval<overload_set<Signatures>>()(
            std::declval<Args>()...));
        set_value_impl<tuple>(
            static_cast<Receiver&&>(r),
            std::make_index_sequence<std::tuple_size_v<tuple>>{},
            static_cast<Args&&>(args)...);
    }
} // namespace detail

//=============================================================================
// AsioSender — stdexec sender 模板
//=============================================================================

template <typename Signatures, typename Initiation, typename... Args>
class AsioSender
{
public:
    using sender_concept = stdexec::sender_t;

private:
    using args_type_ = std::tuple<std::decay_t<Args>...>;

    Initiation init_;
    args_type_ args_;

    template <typename Receiver>
    struct OperationState
    {
        Receiver                         rcvr_;
        Initiation                       init_;
        args_type_                       args_;
        boost::asio::cancellation_signal signal_;
        bool                             completed_ = false;

        // stop_callback：receiver env 的 stop_token -> asio terminal 取消
        struct StopCb
        {
            boost::asio::cancellation_signal& signal_;

            void operator()() && noexcept
            {
                signal_.emit(boost::asio::cancellation_type::terminal);
            }
        };

        using stop_callback_type = stdexec::stop_callback_for_t<
            stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>,
            StopCb>;

        // completion_handler：co_spawn 据此安装 cancellation handler（镜像
        // completion_token.hpp:216-283），operator() 区分错误/取消/值三通道
        struct completion_handler
        {
            OperationState* self_;

            explicit completion_handler(OperationState& self) noexcept
                : self_(&self)
            {
            }

            completion_handler(completion_handler&& other) noexcept
                : self_(std::exchange(other.self_, nullptr))
            {
            }

            completion_handler(const completion_handler&) = delete;
            completion_handler& operator=(const completion_handler&) = delete;

            ~completion_handler() noexcept = default;

            using cancellation_slot_type = boost::asio::cancellation_slot;

            auto get_cancellation_slot() const noexcept
            {
                return self_->signal_.slot();
            }

            template <typename... ValueArgs>
            void operator()(std::exception_ptr e, ValueArgs&&... args) noexcept
            {
                if (e)
                {
                    // operation_aborted -> set_stopped；普通异常 -> set_error。
                    // 取消与错误必须区分，否则 request_stop 的 terminal 取消
                    // 会被当错误传播，scope.spawn 的 set_error 会 rethrow
                    // terminate。
                    if (IsOperationAborted(e))
                    {
                        self_->completed_ = true;
                        stdexec::set_stopped(std::move(self_->rcvr_));
                    }
                    else
                    {
                        self_->completed_ = true;
                        stdexec::set_error(
                            std::move(self_->rcvr_),
                            std::move(e));
                    }
                }
                else
                {
                    self_->completed_ = true;
                    try
                    {
                        detail::set_value<Signatures>(
                            std::move(self_->rcvr_),
                            std::forward<ValueArgs>(args)...);
                    }
                    catch (...)
                    {
                        stdexec::set_error(
                            std::move(self_->rcvr_),
                            std::current_exception());
                    }
                }
                // 注意：这里不能写 self_ = nullptr。完成 receiver 可能同步销毁
                // 持有本 handler 的 OperationState（如 async_scope::spawn 的
                // spawn receiver 完成时会 delete 整个 spawn op，连带 co_spawn
                // 协程帧与其中的本 handler），随后对 self_
                // 的写入即悬空写。handler 恰好只被调用一次（co_spawn
                // 只完成一次），move 构造已把 moved-from 源置空，故无需此行。
            }

        private:
            static bool IsOperationAborted(const std::exception_ptr& e) noexcept
            {
                try
                {
                    std::rethrow_exception(e);
                }
                catch (const boost::system::system_error& se)
                {
                    return se.code() == boost::asio::error::operation_aborted;
                }
                catch (...)
                {
                    return false;
                }
            }
        };

        std::optional<stop_callback_type> stop_cb_;

        template <typename R, typename I, typename A>
        OperationState(R&& rcvr, I&& init, A&& args)
            : rcvr_(std::forward<R>(rcvr)), init_(std::forward<I>(init)),
              args_(std::forward<A>(args))
        {
        }

        void start() & noexcept
        {
            std::apply(
                [this](auto&&... a)
                {
                    std::invoke(
                        static_cast<Initiation&&>(init_),
                        completion_handler{*this},
                        static_cast<decltype(a)&&>(a)...);
                },
                std::move(args_));

            // 先启动 asio op（co_spawn 的 cancellation handler 在 init 期间连接
            // 到 signal_ 的 slot），再安装 stop_callback —— 这样 stop_token 在
            // start() 之前已被 request 时，emit 也能到达 co_spawn 的 handler。
            // 同步完成时 completed_ 已被 handler 置位，跳过安装（op 已完成）。
            if (!completed_)
            {
                stop_cb_.emplace(
                    stdexec::get_stop_token(stdexec::get_env(rcvr_)),
                    StopCb{signal_});
            }
        }
    };

public:
    template <typename T, typename... Us>
        requires std::constructible_from<Initiation, T>
                     && std::constructible_from<args_type_, Us...>
    explicit constexpr AsioSender(T&& t, Us&&... us) noexcept(
        std::is_nothrow_constructible_v<Initiation, T>
        && std::is_nothrow_constructible_v<args_type_, Us...>)
        : init_(static_cast<T&&>(t)), args_(static_cast<Us&&>(us)...)
    {
    }

    template <typename Env>
    constexpr Signatures get_completion_signatures(const Env&) const
    {
        return {};
    }

    template <typename Receiver>
    constexpr auto connect(Receiver&& rcvr) && noexcept(
        std::is_nothrow_constructible_v<
            OperationState<std::remove_cvref_t<Receiver>>,
            Receiver,
            Initiation,
            args_type_>)
    {
        return OperationState<std::remove_cvref_t<Receiver>>(
            static_cast<Receiver&&>(rcvr),
            static_cast<Initiation&&>(init_),
            static_cast<args_type_&&>(args_));
    }
};

DAS_CORE_IPC_NS_END

//=============================================================================
// boost::asio 命名空间特化：async_result<AsioSenderToken, ...>::initiate
// 返回 AsioSender（镜像 completion_token.hpp:493-544）
//=============================================================================

namespace boost::asio
{
    template <typename... Signatures>
    struct async_result<DAS::Core::IPC::AsioSenderToken, Signatures...>
    {
        template <typename Initiation, typename... Args>
        static auto initiate(
            Initiation&& i,
            const DAS::Core::IPC::AsioSenderToken&,
            Args&&... args)
            -> DAS::Core::IPC::AsioSender<
                DAS::Core::IPC::detail::completion_signatures<Signatures...>,
                std::remove_cvref_t<Initiation>,
                Args...>
        {
            return DAS::Core::IPC::AsioSender<
                DAS::Core::IPC::detail::completion_signatures<Signatures...>,
                std::remove_cvref_t<Initiation>,
                Args...>(
                static_cast<Initiation&&>(i),
                static_cast<Args&&>(args)...);
        }
    };
} // namespace boost::asio

#endif // DAS_CORE_IPC_ASIO_SENDER_H
