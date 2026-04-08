// DirectorLifecycle.i — C# bridge lifecycle support
// Provides DasSwigPreventManagedObject and DasSwigReleaseManagedObject
// for C# director objects.
//
// These functions are called from the bridge core (das_swig_generator.py)
// when a director object crosses the native boundary.
//
// The C# side injects __das_bridge_prevent / __das_bridge_release methods
// via swig_csharp_generator.py. These methods use GCHandle to prevent
// premature garbage collection of the managed object.
//
// Dependencies: None (pure C function pointers via SWIGSTDCALL)

#ifdef SWIGCSHARP
%{

// C# bridge lifecycle implementation
// Calls the csharp_prevent / csharp_release function pointers stored in the
// runtime context. These pointers are bound via DasBindCSharpRuntimeContext()
// which receives delegates from the C# __das_bridge_prevent/__das_bridge_release
// methods injected by swig_csharp_generator.py.

static int DasSwigPreventManagedObject(DasSwigRuntimeContext* p_context)
{
    if (!p_context || p_context->kind != DasSwigRuntimeKind::CSharp)
    {
        return -1;
    }
    if (p_context->csharp_prevent)
    {
        return p_context->csharp_prevent();
    }
    return -1;
}

static int DasSwigReleaseManagedObject(DasSwigRuntimeContext* p_context)
{
    if (!p_context || p_context->kind != DasSwigRuntimeKind::CSharp)
    {
        return -1;
    }
    if (p_context->csharp_release)
    {
        return p_context->csharp_release();
    }
    return -1;
}

%}
#endif // SWIGCSHARP
