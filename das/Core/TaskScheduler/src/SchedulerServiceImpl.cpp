#include <das/Core/TaskScheduler/SchedulerServiceImpl.h>
#include <das/DasExport.h>
#include <new>

namespace Das::Core::TaskScheduler
{

    SchedulerServiceImpl::SchedulerServiceImpl(SchedulerService& svc)
        : svc_(svc)
    {
    }

    uint32_t DAS_STD_CALL SchedulerServiceImpl::AddRef()
    {
        return ++ref_count_;
    }

    uint32_t DAS_STD_CALL SchedulerServiceImpl::Release()
    {
        auto count = --ref_count_;
        if (count == 0)
        {
            delete this;
        }
        return count;
    }

    DasResult DAS_STD_CALL
    SchedulerServiceImpl::QueryInterface(const DasGuid& iid, void** pp_out)
    {
        if (pp_out == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        if (iid == DasIidOf<IDasBase>())
        {
            *pp_out = static_cast<IDasBase*>(this);
            AddRef();
            return DAS_S_OK;
        }

        if (iid == DasIidOf<IDasSchedulerService>())
        {
            *pp_out = static_cast<IDasSchedulerService*>(this);
            AddRef();
            return DAS_S_OK;
        }

        *pp_out = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    DasResult SchedulerServiceImpl::Initialize(
        const std::filesystem::path& plugin_dir,
        const std::vector<DasGuid>&  disabled_guids)
    {
        return svc_.Initialize(plugin_dir, disabled_guids);
    }

    DasResult SchedulerServiceImpl::Enable() { return svc_.Enable(); }

    DasResult SchedulerServiceImpl::Disable() { return svc_.Disable(); }

    IDasSchedulerService::SchedulerState SchedulerServiceImpl::Status() const
    {
        return svc_.Status();
    }

} // namespace Das::Core::TaskScheduler

DAS_C_API DasResult CreateDasSchedulerService(
    Das::Core::TaskScheduler::SchedulerService& mgr,
    IDasSchedulerService**                      pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* impl = new Das::Core::TaskScheduler::SchedulerServiceImpl(mgr);
        impl->AddRef();
        *pp_out = impl;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}
