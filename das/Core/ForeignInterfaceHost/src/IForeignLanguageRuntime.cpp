#include "CppHost.h"
#include "JavaHost.h"
#include "LuaHost.h"
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Core/ForeignInterfaceHost/PythonHost.h>
#include <das/Utils/UnexpectedEnumException.h>
#include <tl/expected.hpp>

#include <vector>

#ifdef DAS_EXPORT_CSHARP
#include "CSharpHost.h"
#endif

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    // 语言 runtime 进程级常驻注册表（方案 A：永不卸载）。
    //
    // 动机：语言 runtime（CppRuntime.plugin_lib_ / LuaRuntime.lua_state_ /
    // CSharpHost 的 hostfxr
    // 等）持有底层资源，而插件对象会逃逸进长期存活的对象图 （主进程的
    // PluginManager/TaskScheduler，或 DasHost 的 static g_runtime）。 若
    // runtime 在引用仍存在时被拆卸（FreeLibrary/lua_close/hostfxr_close）即
    // use-after-free。这与 Node.js native addon / Python C
    // 扩展是同一类问题——它们的 共同结论是：原生 runtime
    // 一旦创建即随进程常驻、永不卸载，由 OS 在进程退出回收。
    //
    // DasHost 自身的 static g_runtime 已是该模式的范本；本注册表把它统一到
    // CreateForeignLanguageRuntime 这个主进程与 DasHost 共用的
    // chokepoint，使所有 语言 runtime（无论哪个进程实例化）都永不拆卸。
    //
    // B2 实现：有意永不析构（leaked singleton）。不采用会在进程退出时析构的
    // Meyer's static——那会触发 FreeLibrary/lua_close，而退出期 static
    // 析构顺序未定义，若持有 插件对象的 static 晚析构即退出崩溃。leaked
    // 单例的析构永不执行 → 持有的 DasPtr 永不 Release → runtime 永不被拆卸；OS
    // 进程退出统一回收映射，与 Node 行为一致。
    class NeverUnloadRuntimeRegistry
    {
    public:
        static NeverUnloadRuntimeRegistry& Instance()
        {
            // 有意泄漏：new 出来永不 delete，析构永不执行（见上文 B2 说明）。
            static NeverUnloadRuntimeRegistry* self =
                new NeverUnloadRuntimeRegistry();
            return *self;
        }

        void Retain(DAS::DasPtr<IForeignLanguageRuntime> runtime)
        {
            if (runtime)
            {
                runtimes_.push_back(std::move(runtime));
            }
        }

    private:
        std::vector<DAS::DasPtr<IForeignLanguageRuntime>> runtimes_;
    };
} // namespace

auto CreateForeignLanguageRuntime(
    const ForeignLanguageRuntimeFactoryDesc& desc_base)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>
{
    DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>> result{
        tl::make_unexpected(DAS_E_NO_IMPLEMENTATION)};
    switch (desc_base.language)
    {
        using enum ForeignInterfaceLanguage;
    case Python:
#ifdef DAS_EXPORT_PYTHON
        result = PythonHost::CreateForeignLanguageRuntime(desc_base);
#endif // DAS_EXPORT_PYTHON
        break;
    case CSharp:
#ifdef DAS_EXPORT_CSHARP
        result = CSharpHost::CreateForeignLanguageRuntime(desc_base);
#endif // DAS_EXPORT_CSHARP
        break;
    case Java:
#ifdef DAS_EXPORT_JAVA
        result = JavaHost::CreateJavaRuntime(desc_base);
#endif // DAS_EXPORT_JAVA
        break;
    case Lua:
#ifdef DAS_EXPORT_LUA
        result = LuaHost::CreateForeignLanguageRuntime(desc_base);
#endif // DAS_EXPORT_LUA
        break;
    case Cpp:
        result = CppHost::CreateForeignLanguageRuntime(desc_base);
        break;
    case Node:
        // Node 走外进程（spawn node.exe），主进程无内嵌 runtime，维持
        // NO_IMPLEMENTATION（与原 goto on_no_interface 行为一致）。
        break;
    default:
        throw DAS::Utils::UnexpectedEnumException::FromEnum(desc_base.language);
    }

    // 方案 A：成功的 runtime 进程级常驻、永不卸载（见
    // NeverUnloadRuntimeRegistry）。 retain 后即便调用方（如主进程
    // LocalRuntimeProvider）释放其引用，注册表仍持有 → runtime
    // 及底层资源（DLL/lua_State/hostfxr/解释器）永不被拆卸。
    if (result.has_value() && result.value())
    {
        NeverUnloadRuntimeRegistry::Instance().Retain(result.value());
    }
    return result;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
