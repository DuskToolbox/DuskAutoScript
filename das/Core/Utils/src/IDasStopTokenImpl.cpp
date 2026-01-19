#include <das/Core/Utils/IDasStopTokenImpl.h>

DAS_CORE_UTILS_NS_BEGIN

uint32_t  DasStopTokenImplOnStack::AddRef() { return 1; }

uint32_t  DasStopTokenImplOnStack::Release() { return 1; }

DasResult DasStopTokenImplOnStack::StopRequested()
{
    return is_stop_requested_ ? DAS_TRUE : DAS_FALSE;
}

void DasStopTokenImplOnStack::RequestStop() { is_stop_requested_ = true; }

void DasStopTokenImplOnStack::Reset() { is_stop_requested_ = false; }

DAS_CORE_UTILS_NS_END