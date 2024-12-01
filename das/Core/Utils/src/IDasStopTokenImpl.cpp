#include <das/Core/Utils/IDasStopTokenImpl.h>

DAS_CORE_UTILS_NS_BEGIN

IDasStopTokenImplOnStack::IDasStopTokenImplOnStack(
    DasStopTokenImplOnStack& impl)
    : impl_{impl}
{
}

inline int64_t IDasStopTokenImplOnStack::AddRef() { return impl_.AddRef(); }

inline int64_t IDasStopTokenImplOnStack::Release() { return impl_.Release(); }

inline DasResult IDasStopTokenImplOnStack::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    return impl_.QueryInterface(iid, pp_object);
}

inline DasBool IDasStopTokenImplOnStack::StopRequested()
{
    return impl_.StopRequested();
}

IDasSwigStopTokenImplOnStack::IDasSwigStopTokenImplOnStack(
    DasStopTokenImplOnStack& impl)
    : impl_{impl}
{
}

inline int64_t IDasSwigStopTokenImplOnStack::AddRef() { return impl_.AddRef(); }

inline int64_t IDasSwigStopTokenImplOnStack::Release()
{
    return impl_.Release();
}

inline DasRetSwigBase IDasSwigStopTokenImplOnStack::QueryInterface(
    const DasGuid& iid)
{
    DasRetSwigBase qi_result;
    qi_result.error_code = impl_.QueryInterface(iid, &qi_result.value);
    return qi_result;
}

inline DasBool IDasSwigStopTokenImplOnStack::StopRequested()
{
    return impl_.StopRequested();
}

DAS_CORE_UTILS_NS_END