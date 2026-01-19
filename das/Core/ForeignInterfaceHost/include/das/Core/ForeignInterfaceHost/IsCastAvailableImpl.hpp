#ifndef DAS_CORE_FOREIGNINTERFACEHOST_ISCASTAVAILABLE_HPP
#define DAS_CORE_FOREIGNINTERFACEHOST_ISCASTAVAILABLE_HPP

#include "das/IDasBase.h"
#include <DAS/_autogen/idl/abi/IDasCapture.h>
#include <DAS/_autogen/idl/abi/IDasErrorLens.h>
#include <DAS/_autogen/idl/abi/IDasTask.h>
#include <DAS/_autogen/idl/abi/IDasPluginPackage.h>
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

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_ISCASTAVAILABLE_HPP
