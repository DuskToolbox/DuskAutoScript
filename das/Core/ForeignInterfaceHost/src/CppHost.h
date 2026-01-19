#ifndef DAS_CORE_FOREIGNINTERFACEHOST_CPPINTERFACE_H
#define DAS_CORE_FOREIGNINTERFACEHOST_CPPINTERFACE_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/Plugin.h>

#define DAS_NS_CPPHOST_BEGIN                                                   \
    namespace CppHost                                                          \
    {

#define DAS_NS_CPPHOST_END }

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_CPPHOST_BEGIN

auto CreateForeignLanguageRuntime(const ForeignLanguageRuntimeFactoryDesc& desc)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>;

DAS_NS_CPPHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_CPPINTERFACE_H
