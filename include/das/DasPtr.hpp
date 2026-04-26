#ifndef DAS_DASPTR_HPP
#define DAS_DASPTR_HPP

#include <cstddef>
#include <das/DasConfig.h>
#include <das/DasGuidHolder.h>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>
#include <functional>
#include <utility>

DAS_NS_BEGIN

struct take_ownership_t
{
};

constexpr auto take_ownership = take_ownership_t{};

template <class T>
class DasPtr
{
    template <class U>
    friend class DasPtr;

protected:
    T* ptr_;

    void InternalAddRef() const
    {
        if (ptr_)
        {
            // 显式丢弃返回值以避免 [[nodiscard]] 警告
            static_cast<void>(ptr_->AddRef());
        }
    }

    void InternalRelease() const
    {
        if (ptr_)
        {
            // 显式丢弃返回值以避免 [[nodiscard]] 警告
            static_cast<void>(ptr_->Release());
        }
    }

    template <class U>
    void InternalCopy(U* p_other)
    {
        if (p_other != ptr_)
        {
            InternalRelease();
            ptr_ = p_other;
            InternalAddRef();
        }
    }

public:
    DasPtr() noexcept : ptr_(nullptr) {}
    DasPtr(decltype(nullptr)) noexcept : ptr_(nullptr) {}
    DasPtr(T* p) : ptr_(p) { InternalAddRef(); }
    DasPtr(const DasPtr& other) : ptr_(other.Get()) { InternalAddRef(); }
    template <class U>
    DasPtr(const DasPtr<U>& other) : ptr_(other.Get())
    {
        InternalAddRef();
    }
    DasPtr& operator=(const DasPtr& other)
    {
        InternalCopy(other.ptr_);
        return *this;
    }
    template <class U>
    DasPtr& operator=(const DasPtr<U>& other)
    {
        InternalCopy(other.ptr_);
        return *this;
    }
    DasPtr(DasPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr))
    {
    }
    template <class U>
    DasPtr(DasPtr<U>&& other) noexcept
        : ptr_(std::exchange(other.ptr_, nullptr))
    {
    }
    DasPtr& operator=(DasPtr&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            InternalRelease();
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }
    template <class U>
    DasPtr& operator=(DasPtr<U>&& other) noexcept
    {
        InternalRelease();
        ptr_ = std::exchange(other.ptr_, nullptr);
        return *this;
    }
    ~DasPtr() noexcept { InternalRelease(); }
    [[nodiscard]]
    T* operator->() const noexcept
    {
        return ptr_;
    }
    [[nodiscard]]
    T& operator*() const noexcept
    {
        return *ptr_;
    }
    [[nodiscard]]
    bool operator==(const DasPtr<T>& other) const noexcept
    {
        return ptr_ == other.ptr_;
    }
    [[nodiscard]]
    explicit operator bool() const noexcept
    {
        return Get() != nullptr;
    }
    template <class Other>
    [[nodiscard]]
    Other* As(const DasGuid& id) const
    {
        void* result = nullptr;
        if (ptr_)
        {
            const auto qi_result = ptr_->QueryInterface(id, &result);
            if (DAS::IsFailed(qi_result))
            {
                return nullptr;
            }
        }
        return static_cast<Other*>(result);
    }
    template <class Other>
    [[nodiscard]]
    DasResult As(DasPtr<Other>& other) const
    {
        void* result = nullptr;
        if (ptr_)
        {
            const auto query_interface_result =
                ptr_->QueryInterface(DasIidOf<Other>(), &result);
            if (DAS::IsFailed(query_interface_result))
            {
                return query_interface_result;
            }
            other = DasPtr<Other>::Attach(static_cast<Other*>(result));
            return DAS_S_OK;
        }
        return DAS_E_INVALID_POINTER;
    }
    template <class Other>
    [[nodiscard]]
    DasResult As(Other** pp_out_other) const
    {
        if (ptr_)
        {
            const auto qi_result = ptr_->QueryInterface(
                DasIidOf<Other>(),
                reinterpret_cast<void**>(pp_out_other));
            if (DAS::IsFailed(qi_result))
            {
                return qi_result;
            }
            return DAS_S_OK;
        }
        return DAS_E_NO_INTERFACE;
    }
    T* Reset()
    {
        InternalRelease();
        return std::exchange(ptr_, nullptr);
    }
    [[nodiscard]]
    T* Get() const noexcept
    {
        return ptr_;
    }
    T** Put()
    {
        InternalRelease();
        ptr_ = nullptr;
        return &ptr_;
    }
    void**      PutVoid() { return reinterpret_cast<void**>(Put()); }
    friend void swap(DasPtr& lhs, DasPtr& rhs) noexcept
    {
        std::swap(lhs.ptr_, rhs.ptr_);
    }
    [[nodiscard]]
    auto operator<=>(const DasPtr& other) const noexcept
    {
        return other.ptr_ <=> ptr_;
    };
    [[nodiscard]]
    static DasPtr Attach(T* p)
    {
        DasPtr result{nullptr};
        *result.Put() = p;
        return result;
    }
};

template <class T, class... Args>
auto MakeDasPtr(Args&&... args)
{
    return DasPtr<T>(new T(std::forward<Args>(args)...));
}

template <class Base, class T, class... Args>
auto MakeDasPtr(Args&&... args)
{
    return DasPtr<Base>(new T(std::forward<Args>(args)...));
}

#if __cplusplus >= 201703L
template <class T>
DasPtr(T*) -> DasPtr<T>;
#endif // __cplusplus >= 201703L

/**
 * @brief Non-copyable smart pointer for COM out-parameter ownership pattern.
 *
 * Binds to caller's T** out-parameter at construction (writes nullptr).
 * The object lives directly at *pp_out; destructor Releases it automatically.
 * Call Keep() on success paths to relinquish ownership to the caller.
 * Forgetting Keep() is safe: object is Released, caller gets nullptr.
 */
template <class T>
class DasOutPtr
{
    T** pp_out_;

public:
    /**
     * @brief Bind to caller's out-parameter. Initializes *pp_out to nullptr.
     */
    explicit DasOutPtr(T** pp_out) noexcept : pp_out_(pp_out)
    {
        if (pp_out_)
        {
            *pp_out_ = nullptr;
        }
    }

    DasOutPtr(const DasOutPtr&) = delete;
    DasOutPtr& operator=(const DasOutPtr&) = delete;

    DasOutPtr(DasOutPtr&& other) noexcept
        : pp_out_(std::exchange(other.pp_out_, nullptr))
    {
    }

    DasOutPtr& operator=(DasOutPtr&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            if (pp_out_ && *pp_out_)
            {
                static_cast<void>((*pp_out_)->Release());
                *pp_out_ = nullptr;
            }
            pp_out_ = std::exchange(other.pp_out_, nullptr);
        }
        return *this;
    }

    /**
     * @brief Releases *pp_out if still bound.
     */
    ~DasOutPtr() noexcept
    {
        if (pp_out_ && *pp_out_)
        {
            static_cast<void>((*pp_out_)->Release());
            *pp_out_ = nullptr;
        }
    }

    /**
     * @brief Returns T** for factory out-parameter usage. Releases current
     * pointer first.
     */
    T** Put() noexcept
    {
        if (pp_out_ && *pp_out_)
        {
            static_cast<void>((*pp_out_)->Release());
            *pp_out_ = nullptr;
        }
        return pp_out_;
    }

    /**
     * @brief Confirm the result and relinquish ownership. Destructor will not
     * Release.
     */
    void Keep() noexcept { pp_out_ = nullptr; }

    [[nodiscard]]
    T* Get() const noexcept
    {
        return pp_out_ ? *pp_out_ : nullptr;
    }
    [[nodiscard]]
    T* operator->() const noexcept
    {
        return *pp_out_;
    }
    [[nodiscard]]
    T& operator*() const noexcept
    {
        return **pp_out_;
    }
    [[nodiscard]]
    explicit operator bool() const noexcept
    {
        return pp_out_ && *pp_out_;
    }
};

#if __cplusplus >= 201703L
template <class T>
DasOutPtr(T**) -> DasOutPtr<T>;
#endif // __cplusplus >= 201703L

DAS_NS_END

template <class T>
struct std::hash<DAS::DasPtr<T>>
{
    size_t operator()(const DAS::DasPtr<T>& ptr) const
    {
        return std::hash<T*>()(ptr.Get());
    }
};

#endif // DAS_DASPTR_HPP
