#ifndef DAS_CORE_FOREIGNINTERFACEHOST_JAVAHOST_H
#define DAS_CORE_FOREIGNINTERFACEHOST_JAVAHOST_H

#include <filesystem>
#ifdef DAS_EXPORT_JAVA

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <memory>


#define DAS_NS_JAVAHOST_BEGIN                                                 \
    namespace JavaHost                                                         \
    {

#define DAS_NS_JAVAHOST_END }

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_JAVAHOST_BEGIN

struct JavaRuntimeDesc : public ForeignLanguageRuntimeFactoryDesc
{
    std::filesystem::path jvm_dll_path;
};

DAS_NS_JAVAHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_JAVA

#endif // DAS_CORE_FOREIGNINTERFACEHOST_JAVAHOST_H
