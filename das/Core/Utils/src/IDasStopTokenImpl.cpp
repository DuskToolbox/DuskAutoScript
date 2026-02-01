#include <das/Core/Utils/IDasStopTokenImpl.h>

DAS_CORE_UTILS_NS_BEGIN

uint32_t DasStopTokenImplOnStack::AddRef() { return 1; }

uint32_t DasStopTokenImplOnStack::Release() { return 1; }

DasResult DasStopTokenImplOnStack::StopRequested(bool* canStop)
{
    if (canStop == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *canStop = is_stop_requested_;
    return DAS_S_OK;
}

void DasStopTokenImplOnStack::RequestStop() { is_stop_requested_ = true; }

void DasStopTokenImplOnStack::Reset() { is_stop_requested_ = false; }

DAS_CORE_UTILS_NS_END