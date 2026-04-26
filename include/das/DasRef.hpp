#pragma once

#include <das/DasPtr.hpp>
#include <stdexcept>
#include <utility>

DAS_NS_BEGIN

template <class T>
class DasCRef;

/**
 * @brief Non-nullable COM smart reference wrapper
 *
 * Wraps DasPtr<T> internally for COM AddRef/Release RAII,
 * with non-null guarantee and reference-style access.
 * Modeled after DasSharedRef but uses COM refcounting via DasPtr.
 *
 * Use for: mandatory constructor-injected COM dependencies
 * that are guaranteed non-null and share lifecycle via AddRef/Release.
 */
template <class T>
class DasRef
{
public:
    DasRef() = delete;
    DasRef(std::nullptr_t) = delete;

    /**
     * @brief Construct from raw pointer. Validates non-null, AddRef.
     */
    explicit DasRef(T* p) : ptr_(p)
    {
        if (!ptr_)
        {
            throw std::invalid_argument{"DasRef requires non-null object"};
        }
    }

    /**
     * @brief Construct from DasPtr, assuming shared ownership of the
     * reference held by the DasPtr. Validates non-null.
     */
    explicit DasRef(DasPtr<T>&& p) : ptr_(std::move(p))
    {
        if (!ptr_)
        {
            throw std::invalid_argument{"DasRef requires non-null DasPtr"};
        }
    }

    DasRef(const DasRef&) = default;
    DasRef& operator=(const DasRef&) = default;
    DasRef(DasRef&&) noexcept = default;
    DasRef& operator=(DasRef&&) noexcept = default;
    ~DasRef() = default;

    operator T&() const noexcept { return *ptr_; }

    [[nodiscard]]
    T& get() const noexcept
    {
        return *ptr_;
    }

    /**
     * @brief Access underlying DasPtr for QI interop (e.g., As<Other>())
     */
    [[nodiscard]]
    const DasPtr<T>& as_ptr() const noexcept
    {
        return ptr_;
    }

private:
    DasPtr<T> ptr_;
};

/**
 * @brief Non-nullable const view of a DasRef
 *
 * Constructible from DasRef<T>. Does NOT AddRef/Release —
 * it borrows the pointer. Must not outlive the source DasRef.
 */
template <class T>
class DasCRef
{
public:
    DasCRef() = delete;
    DasCRef(std::nullptr_t) = delete;

    explicit DasCRef(const DasRef<T>& ref) noexcept : ptr_(&ref.get()) {}

    DasCRef(const DasCRef&) = default;
    DasCRef& operator=(const DasCRef&) = default;
    ~DasCRef() = default;

    operator const T&() const noexcept { return *ptr_; }

    [[nodiscard]]
    const T& get() const noexcept
    {
        return *ptr_;
    }

private:
    const T* ptr_;
};

#if __cplusplus >= 201703L
template <class T>
DasRef(T*) -> DasRef<T>;
#endif // __cplusplus >= 201703L

DAS_NS_END
