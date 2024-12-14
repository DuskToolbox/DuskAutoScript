#ifndef DAS_CORE_FOREIGNINTERFACEHOST_ISCASTAVAILABLE_HPP
#define DAS_CORE_FOREIGNINTERFACEHOST_ISCASTAVAILABLE_HPP

#include "das/IDasBase.h"
#include <das/PluginInterface/IDasCapture.h>
#include <das/PluginInterface/IDasErrorLens.h>
#include <das/PluginInterface/IDasTask.h>
#include <das/PluginInterface/IDasPluginPackage.h>
#include <das/DasString.hpp>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <type_traits>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

template <class... Ts>
class SwigTypeInheritChain
{
};

template <class... Ts>
DasResult IsCastAvailableImpl(const DasGuid& iid, SwigTypeInheritChain<Ts...>)
{
    if (((iid == DasIidOf<Ts>()) || ...))
    {
        return DAS_S_OK;
    }
    else
    {
        return DAS_E_NO_INTERFACE;
    }
}

using das_task_inherit_chain = SwigTypeInheritChain<IDasSwigBase, IDasSwigTask>;
using das_capture_inherit_chain =
    SwigTypeInheritChain<IDasSwigBase, IDasSwigCapture>;
using das_plugin_inherit_chain =
    SwigTypeInheritChain<IDasSwigBase, IDasSwigPluginPackage>;

template <class EndT, class Arg, class... ResultTs>
auto SwigTypeInheritChainEndOfImpl(
    SwigTypeInheritChain<Arg>,
    SwigTypeInheritChain<ResultTs...>) -> SwigTypeInheritChain<ResultTs..., Arg>
{
    return {};
}

template <class EndT, class Arg, class... Args, class... ResultTs>
auto SwigTypeInheritChainEndOfImpl(
    SwigTypeInheritChain<Arg, Args...>,
    SwigTypeInheritChain<ResultTs...>)
    -> std::conditional_t<
        std::is_same_v<EndT, Arg>,
        SwigTypeInheritChain<ResultTs..., Arg>,
        decltype(SwigTypeInheritChainEndOfImpl<EndT>(
            SwigTypeInheritChain<Args...>{},
            SwigTypeInheritChain<ResultTs..., Arg>{}))>
{
    return {};
}

template <class EndT, class Arg, class... Args>
auto SwigTypeInheritChainEndOfImpl(SwigTypeInheritChain<Arg, Args...>)
    -> std::conditional_t<
        std::is_same_v<EndT, Arg>,
        SwigTypeInheritChain<Arg>,
        decltype(SwigTypeInheritChainEndOfImpl<EndT>(
            SwigTypeInheritChain<Args...>{},
            SwigTypeInheritChain<Arg>{}))>
{
    return {};
}

template <class EndT, class TypeList>
using swig_type_inherit_chain_end_of_t =
    decltype(SwigTypeInheritChainEndOfImpl<EndT>(TypeList{}));

template <class T, class Input>
struct swig_type_inherit_chain_add_type;

template <class T, class... Args>
struct swig_type_inherit_chain_add_type<T, SwigTypeInheritChain<Args...>>
{
    using type = SwigTypeInheritChain<Args..., T>;
};

template <class T, class Input>
using swig_type_inherit_chain_add_type_t =
    typename swig_type_inherit_chain_add_type<T, Input>::type;

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_ISCASTAVAILABLE_HPP
