// DasSwigRuntimeTypes.i — Bridge lifecycle type definitions
// Included before DirectorLifecycle.i to provide DasSwigRuntimeContext
// and related types. Each IDL-generated .i file also contains these
// definitions wrapped in #ifndef guards to avoid redefinition.

%{
#ifndef DAS_SWIG_RUNTIME_LIFECYCLE_TYPES_DEFINED
#define DAS_SWIG_RUNTIME_LIFECYCLE_TYPES_DEFINED

enum class DasSwigRuntimeKind
{
    None,
    Java,
    Python,
    CSharp,
};

typedef int (*DasCSharpLifecycleFn)();

struct DasSwigRuntimeContext
{
    DasSwigRuntimeKind kind = DasSwigRuntimeKind::None;
    void* java_vm = nullptr;
    void* java_self = nullptr;        // global ref
    void* py_self = nullptr;          // strong ref
    DasCSharpLifecycleFn csharp_prevent = nullptr;
    DasCSharpLifecycleFn csharp_release = nullptr;
};

#endif // DAS_SWIG_RUNTIME_LIFECYCLE_TYPES_DEFINED
%}
