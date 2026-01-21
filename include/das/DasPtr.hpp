#ifndef DAS_DASPTR_HPP
#define DAS_DASPTR_HPP

#include <das/DasTypes.hpp>
#include <das/Utils/fmt.h>

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
            ptr_->AddRef();
        }
    }

    void InternalRelease() const
    {
        if (ptr_)
        {
            ptr_->Release();
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
    T*   operator->() const noexcept { return ptr_; }
    T&   operator*() const noexcept { return *ptr_; }
    bool operator==(const DasPtr<T>& other) const noexcept
    {
        return ptr_ == other.ptr_;
    }
    explicit operator bool() const noexcept { return Get() != nullptr; }
    template <class Other>
    Other* As(const DasGuid& id) const
    {
        void* result = nullptr;
        if (ptr_)
        {
            ptr_->QueryInterface(id, &result);
            ptr_->AddRef();
        }
        return static_cast<Other*>(result);
    }
    template <class Other>
    DasResult As(DasPtr<Other>& other) const
    {
        void* result = nullptr;
        if (ptr_)
        {
            const auto query_interface_result =
                ptr_->QueryInterface(DasIidOf<Other>(), &result);
            if (IsFailed(query_interface_result))
            {
                return query_interface_result;
            }
            other = {static_cast<Other*>(result)};
            return DAS_S_OK;
        }
        return DAS_E_INVALID_POINTER;
    }
    template <class Other>
    DasResult As(Other** pp_out_other) const
    {
        if (ptr_)
        {
            ptr_->QueryInterface(
                DasIidOf<Other>(),
                reinterpret_cast<void**>(pp_out_other));
            (*pp_out_other)->AddRef();
            return DAS_S_OK;
        }
        return DAS_E_NO_INTERFACE;
    }
    T* Reset()
    {
        InternalRelease();
        return std::exchange(ptr_, nullptr);
    }
    T*  Get() const noexcept { return ptr_; }
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
    auto operator<=>(const DasPtr& other) const noexcept
    {
        return other.ptr_ <=> ptr_;
    };
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

DAS_NS_END

template <class T>
struct std::hash<DAS::DasPtr<T>>
{
    size_t operator()(const DAS::DasPtr<T>& ptr) const
    {
        return std::hash<T*>()(ptr.Get());
    }
};

#if __cplusplus >= 202002L && DAS_USE_STD_FMT
// Formatter specialization for DasPtr<T>
template <class T>
struct DAS_FMT_NS::formatter<DAS::DasPtr<T>, char>
    : public formatter<void*, char>
{
    auto format(const DAS::DasPtr<T>& ptr, auto& ctx) const
    {
        return DAS_FMT_NS::format_to(
            ctx.out(),
            "{}",
            static_cast<void*>(ptr.Get()));
    }
};
#endif

#endif // DAS_DASPTR_HPP
