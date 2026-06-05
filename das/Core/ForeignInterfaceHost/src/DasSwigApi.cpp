#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasSwigApi.h>

// C API  - sole business logic (must appear before SWIG namespace)
DAS_C_API DasResult
DasQueryMainProcessInterface(const DasGuid& iid, IDasBase** pp_out_object)
{
    if (pp_out_object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_object = nullptr;

    auto* ctx = Das::Core::IPC::GetCurrentIpcContext();
    if (!ctx)
    {
        return DAS_E_OBJECT_NOT_INIT;
    }

    return ctx->ResolveMainProcessInterface(iid, pp_out_object);
}

DAS_C_API DasResult
DasRegisterMainProcessService(IDasBase* p_object, const DasGuid& iid)
{
    if (!p_object)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto* ctx = Das::Core::IPC::GetCurrentIpcContext();
    if (!ctx)
    {
        return DAS_E_OBJECT_NOT_INIT;
    }

    return ctx->RegisterService(p_object, iid);
}

DAS_C_API DasResult DasUnregisterMainProcessService(const DasGuid& iid)
{
    auto* ctx = Das::Core::IPC::GetCurrentIpcContext();
    if (!ctx)
    {
        return DAS_E_OBJECT_NOT_INIT;
    }

    return ctx->UnregisterService(iid);
}

// --- By-name variants ---

DAS_C_API DasResult
DasQueryMainProcessInterfaceByName(const char* name, IDasBase** pp_out_object)
{
    if (!name || !pp_out_object)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_object = nullptr;

    auto* ctx = Das::Core::IPC::GetCurrentIpcContext();
    if (!ctx)
    {
        return DAS_E_OBJECT_NOT_INIT;
    }

    return ctx->ResolveMainProcessInterfaceByName(name, pp_out_object);
}

DAS_C_API DasResult DasRegisterMainProcessServiceByName(
    IDasBase*      p_object,
    const DasGuid& iid,
    const char*    name)
{
    if (!p_object || !name)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto* ctx = Das::Core::IPC::GetCurrentIpcContext();
    if (!ctx)
    {
        return DAS_E_OBJECT_NOT_INIT;
    }

    return ctx->RegisterServiceByName(p_object, iid, name);
}

DAS_C_API DasResult DasUnregisterMainProcessServiceByName(const char* name)
{
    if (!name)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto* ctx = Das::Core::IPC::GetCurrentIpcContext();
    if (!ctx)
    {
        return DAS_E_OBJECT_NOT_INIT;
    }

    return ctx->UnregisterServiceByName(name);
}

DAS_C_API DasResult InitializeIDasPluginManager(
    Das::ExportInterface::IDasReadOnlyGuidVector*,
    Das::ExportInterface::IDasInitializeIDasPluginManagerCallback*,
    Das::ExportInterface::IDasInitializeIDasPluginManagerWaiter** pp_out_waiter)
{
    if (pp_out_waiter == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    *pp_out_waiter = nullptr;
    return DAS_E_NO_IMPLEMENTATION;
}

DAS_C_API uint32_t DasAddRef(IDasBase* p_base)
{
    if (p_base == nullptr)
    {
        return 0;
    }

    return p_base->AddRef();
}

DAS_C_API uint32_t DasRelease(IDasBase* p_base)
{
    if (p_base == nullptr)
    {
        return 0;
    }

    return p_base->Release();
}

DAS_C_API DasResult DispatchIDasComponent(
    Das::PluginInterface::IDasComponent*      p_component,
    IDasReadOnlyString*                       p_function_name,
    Das::ExportInterface::IDasVariantVector*  p_arguments,
    Das::ExportInterface::IDasVariantVector** pp_out_result)
{
    if (p_component == nullptr || p_function_name == nullptr
        || p_arguments == nullptr || pp_out_result == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    return p_component->Dispatch(p_function_name, p_arguments, pp_out_result);
}

// SWIG-compatible wrappers — now at global scope (moved out of Das::Swig)
// Thin wrappers that delegate to C API, returning DasRetXxx value types.

DasRetIDasBase QueryMainProcessInterface(const DasGuid& iid)
{
    DasRetIDasBase result;
    IDasBase*      raw_ptr = nullptr;

    DasResult hr = DasQueryMainProcessInterface(iid, &raw_ptr);
    result.SetErrorCode(hr);

    if (DAS::IsOk(hr) && raw_ptr)
    {
        result.SetValue(raw_ptr);
        // SetValue 会 AddRef，平衡 QueryInterface 返回的原始引用
        raw_ptr->Release();
    }
    return result;
}

DasRetIDasBase QueryMainProcessInterfaceByName(const char* name)
{
    DasRetIDasBase result;
    IDasBase*      raw_ptr = nullptr;

    DasResult hr = DasQueryMainProcessInterfaceByName(name, &raw_ptr);
    result.SetErrorCode(hr);

    if (DAS::IsOk(hr) && raw_ptr)
    {
        result.SetValue(raw_ptr);
        // SetValue 会 AddRef，平衡 QueryInterface 返回的原始引用
        raw_ptr->Release();
    }
    return result;
}
