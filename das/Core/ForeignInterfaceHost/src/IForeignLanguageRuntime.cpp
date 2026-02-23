#include "CppHost.h"
#include "JavaHost.h"
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PythonHost.h>
#include <das/Utils/UnexpectedEnumException.h>
#include <tl/expected.hpp>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

auto CreateForeignLanguageRuntime(
    const ForeignLanguageRuntimeFactoryDesc& desc_base)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>
{
    switch (desc_base.language)
    {
        using enum ForeignInterfaceLanguage;
    case Python:
#ifndef DAS_EXPORT_PYTHON
        goto on_no_interface;
#else
        return PythonHost::CreateForeignLanguageRuntime(desc_base);
#endif // DAS_EXPORT_PYTHON
    case CSharp:
#ifndef DAS_EXPORT_CSHARP
        goto on_no_interface;
#else
#endif // DAS_EXPORT_CSHARP
    case Java:
#ifndef DAS_EXPORT_JAVA
        goto on_no_interface;
#else
#endif // DAS_EXPORT_JAVA
    case Lua:
        goto on_no_interface;
    case Cpp:
        return CppHost::CreateForeignLanguageRuntime(desc_base);
    default:
        throw DAS::Utils::UnexpectedEnumException::FromEnum(desc_base.language);
    }
on_no_interface:
    return tl::make_unexpected(DAS_E_NO_IMPLEMENTATION);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END