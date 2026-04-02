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

DAS_C_API DasResult
DasUnregisterMainProcessService(const DasGuid& iid)
{
    auto* ctx = Das::Core::IPC::GetCurrentIpcContext();
    if (!ctx)
    {
        return DAS_E_OBJECT_NOT_INIT;
    }

    return ctx->UnregisterService(iid);
}
// SWIG version  - thin wrapper, delegates to C API
DAS_SWIG_NS_BEGIN

DasRetIDasBase QueryMainProcessInterface(const DasGuid& iid)
{
    DasRetIDasBase result;
    IDasBase*      raw_ptr = nullptr;

    DasResult hr = DasQueryMainProcessInterface(iid, &raw_ptr);
    result.SetErrorCode(hr);

    if (DAS::IsOk(hr) && raw_ptr)
    {
        result.value = DasPtr<IDasBase>::Attach(raw_ptr);
    }
    return result;
}

DAS_SWIG_NS_END
