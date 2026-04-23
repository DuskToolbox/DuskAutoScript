#pragma once

#include <das/DasConfig.h>
#include <memory>
#include <stdexcept>
#include <utility>

DAS_NS_BEGIN

template <class T>
class DasSharedCRef;

/**
 * @brief 非空共享生命周期引用包装器
 *
 * 包装 std::shared_ptr<T>，保证持有的对象非空。
 * 提供引用风格的访问（operator T&/get()），
 * 适用于构造函数注入的必需依赖。
 *
 * 禁止默认构造和 nullptr 构造。
 * 拷贝共享所有权，移动转移包装器。
 */
template <class T>
class DasSharedRef
{
public:
    using type = T;

    DasSharedRef() = delete;
    DasSharedRef(std::nullptr_t) = delete;

    explicit DasSharedRef(std::shared_ptr<T> object)
        : object_{std::move(object)}
    {
        if (!object_)
        {
            throw std::invalid_argument{
                "DasSharedRef requires non-null object"};
        }
    }

    template <class Deleter>
    DasSharedRef(T* object, Deleter deleter)
        : DasSharedRef{std::shared_ptr<T>{object, std::move(deleter)}}
    {
    }

    operator T&() const noexcept { return *object_; }

    [[nodiscard]]
    T& get() const noexcept
    {
        return *object_;
    }

    [[nodiscard]]
    const std::shared_ptr<T>& shared() const noexcept
    {
        return object_;
    }

private:
    std::shared_ptr<T> object_;
};

/**
 * @brief 非空共享生命周期 const 引用包装器
 *
 * 与 DasSharedRef 类似但持有 const T。
 * 可从 DasSharedRef<T> 隐式构造。
 */
template <class T>
class DasSharedCRef
{
public:
    using type = const T;

    DasSharedCRef() = delete;
    DasSharedCRef(std::nullptr_t) = delete;

    explicit DasSharedCRef(std::shared_ptr<const T> object)
        : object_{std::move(object)}
    {
        if (!object_)
        {
            throw std::invalid_argument{
                "DasSharedCRef requires non-null object"};
        }
    }

    explicit DasSharedCRef(const DasSharedRef<T>& object)
        : object_{object.shared()}
    {
    }

    template <class Deleter>
    DasSharedCRef(const T* object, Deleter deleter)
        : DasSharedCRef{std::shared_ptr<const T>{object, std::move(deleter)}}
    {
    }

    operator const T&() const noexcept { return *object_; }

    [[nodiscard]]
    const T& get() const noexcept
    {
        return *object_;
    }

    [[nodiscard]]
    const std::shared_ptr<const T>& shared() const noexcept
    {
        return object_;
    }

private:
    std::shared_ptr<const T> object_;
};

DAS_NS_END
