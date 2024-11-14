#ifndef DAS_UTILS_STREAMUILS_HPP
#define DAS_UTILS_STREAMUILS_HPP

#include <das/Utils/Config.h>
#include <utility>
#include <fstream>

DAS_UTILS_NS_BEGIN

template <class Input, class Target>
struct change_template_container;

template <
    template <class...>
    class Input,
    template <class...>
    class Target,
    class... Args,
    class... DiscardArgs>
struct change_template_container<Input<DiscardArgs...>, Target<Args...>>
{
    using type = Target<Args...>;
};

template <class InContainer, class OutContainer>
using change_template_container_t =
    typename change_template_container<InContainer, OutContainer>::type;

template <class T, class F>
    requires std::is_base_of_v<change_template_container_t<T, std::ios>, T>

T& EnableStreamException(T& t, const std::ios_base::iostate state, F&& f)
{
    const auto mask = t.exceptions();
    t.exceptions(state);
    f(t);
    t.exceptions(mask);
    return t;
}

DAS_UTILS_NS_END

#endif // DAS_UTILS_STREAMUILS_HPP
