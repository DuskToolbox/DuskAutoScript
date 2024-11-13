#ifndef ASR_UTILS_UTILS_HPP
#define ASR_UTILS_UTILS_HPP

#include <AutoStarRail/Utils/Config.h>
#include <atomic>
#include <cstring>
#include <thread>
#include <type_traits>

ASR_UTILS_NS_BEGIN

template <class E>
constexpr auto ToUnderlying(E e) noexcept -> std::underlying_type_t<E>
{
    return static_cast<std::underlying_type_t<E>>(e);
}

inline void* VoidP(void* pointer) { return pointer; }

/**
 * @brief 示例： static_assert(value<false, T>, "你需要的说明");
 */
template <auto A, typename...>
const auto value = A;

#define ASR_UTILS_CHECK_POINTER(pointer)                                       \
    if (pointer == nullptr) [[unlikely]]                                       \
    {                                                                          \
        ASR_CORE_LOG_ERROR("Null pointer found! Variable name is " #pointer    \
                           ". Please check your code.");                       \
        return ASR_E_INVALID_POINTER;                                          \
    }

#define ASR_UTILS_CHECK_POINTER_FOR_PLUGIN(pointer)                            \
    if (pointer == nullptr) [[unlikely]]                                       \
    {                                                                          \
        ASR_LOG_ERROR("Null pointer found! Variable name is " #pointer         \
                      ". Please check your code.");                            \
        return ASR_E_INVALID_POINTER;                                          \
    }

#define ASR_UTILS_IASRBASE_AUTO_IMPL(class_name)                               \
private:                                                                       \
    ASR::Utils::RefCounter<class_name> ref_counter_{};                         \
                                                                               \
public:                                                                        \
    int64_t AddRef() override { return ref_counter_.AddRef(); }                \
    int64_t Release() override { return ref_counter_.Release(this); }

#define ASR_UTILS_GET_RUNTIME_CLASS_NAME_IMPL(class_name, pp_out_class_name)   \
    static AsrReadOnlyString result{static_cast<const char*>(                  \
        ASR_UTILS_STRINGUTILS_DEFINE_U8STR(#class_name))};                     \
                                                                               \
    result.GetImpl(pp_out_class_name);                                         \
    return ASR_S_OK

template <class Object, class T>
class ProjectionGenerator
{
    [[no_unique_address]]
    T projection{*static_cast<std::add_pointer_t<Object>>(this)};

public:
    operator std::add_pointer_t<T>() { return &projection; }
};

// clang-format off

#define ASR_UTILS_MULTIPLE_PROJECTION_GENERATORS(                              \
    object_type,                                                               \
    projection_type1,                                                          \
    projection_type2)                                                          \
    public ASR::Utils::ProjectionGenerator<object_type, projection_type1>,     \
    public ASR::Utils::ProjectionGenerator<object_type, projection_type2>

// clang-format on

struct NonCopyable
{
    NonCopyable() = default;
    ~NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

struct NonMovable
{
    NonMovable() = default;
    ~NonMovable() = default;
    NonMovable(NonMovable&&) = delete;
    NonMovable& operator=(NonMovable&&) = delete;
};

struct NonCopyableAndNonMovable
{
    NonCopyableAndNonMovable() = default;
    ~NonCopyableAndNonMovable() = default;
    NonCopyableAndNonMovable(const NonCopyableAndNonMovable&) = delete;
    NonCopyableAndNonMovable& operator=(const NonCopyableAndNonMovable&) =
        delete;
    NonCopyableAndNonMovable(NonCopyableAndNonMovable&&) = delete;
    NonCopyableAndNonMovable& operator=(NonCopyableAndNonMovable&&) = delete;
};

template <
    class T,
    class =
        std::enable_if_t<std::is_standard_layout_v<T> && std::is_trivial_v<T>>>
void CopyArray(T* p_from, std::size_t array_length, void* p_to)
{
    ::memcpy(p_to, p_from, array_length * sizeof(T));
}

template <class OnExitFunc>
class OnExit : public NonCopyableAndNonMovable
{
    ASR_USING_BASE_CTOR(NonCopyableAndNonMovable);

    [[no_unique_address]]
    OnExitFunc on_exit_func_;

public:
    template <class F>
    OnExit(F&& on_exit_func) : on_exit_func_{std::forward<F>(on_exit_func)}
    {
    }

    OnExit() : on_exit_func_{} {}

    ~OnExit() { on_exit_func_(); }
};
template <class OnExitFunc>
OnExit(OnExitFunc) -> OnExit<OnExitFunc>;

template <class T, class OnExitFunc>
class ScopeGuard : public NonCopyableAndNonMovable
{
    ASR_USING_BASE_CTOR(NonCopyableAndNonMovable);

    T value_;
    [[no_unique_address]]
    OnExitFunc on_exit_func_;

public:
    template <class F>
    ScopeGuard(const T& value, F&& on_exit_func)
        : value_{value}, on_exit_func_{std::forward<F>(on_exit_func)}
    {
    }

    template <class F>
    ScopeGuard(const T& value) : value_{value}, on_exit_func_{}
    {
    }

    ~ScopeGuard() { on_exit_func_(value_); }
};

template <class OnExitFunc>
class ScopeGuardVoid : public NonCopyableAndNonMovable
{
    ASR_USING_BASE_CTOR(NonCopyableAndNonMovable);

    [[no_unique_address]]
    OnExitFunc on_exit_func_;

public:
    template <class FInit, class FDestroy>
    ScopeGuardVoid(FInit on_init_func, FDestroy&& on_exit_func)
        : on_exit_func_{std::forward<FDestroy>(on_exit_func)}
    {
        on_init_func();
    }
    template <class FInit>
    ScopeGuardVoid(FInit on_init_func) : on_exit_func_{}
    {
        on_init_func();
    }
    ~ScopeGuardVoid() { on_exit_func_(); }
};
template <class FInit, class FDestroy>
ScopeGuardVoid(FInit, FDestroy&&) -> ScopeGuardVoid<FDestroy>;

template <class T>
class IfNotNull : public NonCopyableAndNonMovable
{
private:
    T value_;

public:
    IfNotNull(T value) : value_{value} {}
    template <class F>
    IfNotNull& operator>>(F f)
    {
        if (value_ != nullptr)
        {
            f(value_);
        }
        return *this;
    }
};

template <class T>
class RefCounter
{
private:
    std::atomic_uint64_t ref_count_{0};

public:
    RefCounter() = default;
    ~RefCounter() = default;
    int64_t AddRef()
    {
        ref_count_ += 1;
        return ref_count_;
    }
    int64_t Release(T* p_managed_object)
    {
        ref_count_ -= 1;
        if (ref_count_ == 0)
        {
            // 避免析构过程中，有对象意外AddRef/Release后导致双重释放
            ref_count_ = 1;
            delete p_managed_object;
            return 0;
        }
        return ref_count_;
    }
};

template <class T>
T MakeObjectAndSetSize()
{
    T result{};
    result.size = sizeof(T);
    return result;
}

template <typename T, class InitFunc>
class Singleton : public NonCopyableAndNonMovable
{
public:
    Singleton() = delete;
    ~Singleton() = delete;

    static T& instance()
    {
        static T object{InitFunc{}()};
        return object;
    }
};

template <class C>
C MakeEmptyCOntainerOfReservedSize(std::size_t reserved_size)
{
    using Allocator = typename C::allocator_type;
    C result{std::size_t{0}, {}, Allocator{}};
    result.reserve(reserved_size);
    return result;
}

template <class C>
C MakeEmptyContainer()
{
    using Allocator = typename C::allocator_type;
    C result{std::size_t{0}, {}, Allocator{}};
    return result;
}

template <class C>
C MakeContainerOfSize(const std::size_t size)
{
    using Allocator = typename C::allocator_type;
    C result{size, {}, Allocator{}};
    return result;
}

template <auto FirstValue, auto... Values>
decltype(FirstValue) get_first_element()
{
    return FirstValue;
}

template <auto... Values>
constexpr bool IsMatchOneOf(const auto value)
{
    if constexpr (std::is_pointer_v<decltype(get_first_element<Values...>())>)
    {
        return ((value == *Values) || ...);
    }
    else
    {
        return ((value == Values) || ...);
    }
}

template <class... Ts>
struct overload_set : Ts...
{
    using Ts::operator()...;
};

template <class... Ts>
overload_set(Ts...) -> overload_set<Ts...>;

class ThreadVerifier
{
public:
    ThreadVerifier() : id_{std::this_thread::get_id()} {}
    void UpdateBindingThread() noexcept { id_ = std::this_thread::get_id(); }
    [[nodiscard]]
    bool IsUnexpectedThread() const noexcept
    {
        return id_ != std::this_thread::get_id();
    }

private:
    std::thread::id id_;
};

/**
 * @brief 将result设置到p_result中，并调用AddRef
 */
template <class R, class T>
void SetResult(R&& result, T* p_result)
{
    constexpr bool has_Get = requires(const R& v) { v.Get(); };
    if constexpr (has_Get)
    {
        (*p_result) = result.Get();
        (*p_result)->AddRef();
    }
    else
    {
        (*p_result) = result;
        (*p_result)->AddRef();
    }
}

ASR_UTILS_NS_END

#endif // ASR_UTILS_UTILS_HPP
