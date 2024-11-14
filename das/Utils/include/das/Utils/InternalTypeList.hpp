#ifndef DAS_UTILS_INTERNALTYPELIST_H
#define DAS_UTILS_INTERNALTYPELIST_H

#include <das/Utils/Config.h>
#include <cstdint>

DAS_UTILS_NS_BEGIN

template <std::size_t I, class T>
struct at_impl;

template <
    std::size_t I,
    template <class...>
    class C,
    class First,
    class... Other>
struct at_impl<I, C<First, Other...>>
{
    using type = typename at_impl<I - 1, C<Other...>>::type;
};

template <template <class...> class C, class First, class... Other>
struct at_impl<0, C<First, Other...>>
{
    using type = First;
};

template <class... Ts>
struct [[maybe_unused]] internal_type_holder
{
    template <std::size_t I>
    using At = typename at_impl<I, internal_type_holder<Ts...>>::type;
    constexpr static std::size_t size = sizeof...(Ts);
};

template <class... LHS, class... RHS>
auto operator+(
    internal_type_holder<LHS...>,
    internal_type_holder<RHS...>) noexcept
    -> internal_type_holder<LHS..., RHS...>
{
    return {};
}

DAS_UTILS_NS_END

#endif // DAS_UTILS_INTERNALTYPELIST_H
