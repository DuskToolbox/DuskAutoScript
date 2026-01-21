#ifdef DAS_EXPORT_JAVA

#include "JavaHost.h"
#include <das/Utils/CommonUtils.hpp>
#include <jni.h>

// CommonPluginPtr is a type alias for DasPtr<IForeignLanguageRuntime>
// It's defined in PythonHost.h (LoadPlugin function's return type)
using CommonPluginPtr = DasPtr<IForeignLanguageRuntime>;

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_JAVAHOST_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

struct JvmDeleter
{
    void operator()(JavaVM* p_vm) const noexcept { p_vm->DestroyJavaVM(); }
};
using UniqueJvmPointer = std::unique_ptr<JavaVM, JvmDeleter>;

DAS_NS_ANONYMOUS_DETAILS_END

class SingletonJvm
{
    static boost::dll::shared_library  jvm_dll_;
    static decltype(&JNI_CreateJavaVM) func_jni_create_jvm_;
    static void LoadJvm(const std::filesystem::path& jvm_path)
    {
        jvm_dll_ = boost::dll::shared_library{jvm_path.c_str()};
        func_jni_create_jvm_ =
            jvm_dll_.get<decltype(JNI_CreateJavaVM)>("JNI_CreateJavaVM");
    }
    // static JavaVM* GetJvm() noexcept {}
};

DAS_DEFINE_VARIABLE(SingletonJvm::jvm_dll_){};
DAS_DEFINE_VARIABLE(SingletonJvm::func_jni_create_jvm_){nullptr};

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
        -> DAS::Utils::Expected<CommonPluginPtr> override;
};

DAS_NS_JAVAHOST_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_JAVA