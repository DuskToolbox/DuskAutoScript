#ifdef DAS_EXPORT_JAVA

#include "JavaHost.h"
#include <boost/dll.hpp>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Utils/CommonUtils.hpp>
#include <jni.h>

using CommonPluginPtr = Das::DasPtr<IDasBase>;

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_JAVAHOST_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

struct JvmDeleter
{
    void operator()(JavaVM* p_vm) const noexcept { p_vm->DestroyJavaVM(); }
};
using UniqueJvmPointer = std::unique_ptr<JavaVM, JvmDeleter>;

DAS_NS_ANONYMOUS_DETAILS_END

class JavaRuntime final : public IForeignLanguageRuntime
{
public:
    JavaRuntime();
    uint32_t  AddRef() override { return 1; };
    uint32_t  Release() override { return 1; };
    DasResult QueryInterface(const DasGuid&, void**) override
    {
        return DAS_E_NO_IMPLEMENTATION;
    }
    auto LoadPlugin(const std::filesystem::path& path)
        -> DAS::Utils::Expected<DasPtr<IDasBase>> override;
};

DAS_NS_JAVAHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_JAVA