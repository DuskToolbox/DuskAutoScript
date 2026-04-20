#include <das/Core/ForeignInterfaceHost/PluginManagerServiceImpl.h>
#include <das/DasExport.h>
#include <new>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

PluginManagerServiceImpl::PluginManagerServiceImpl(PluginManager& mgr)
    : mgr_(mgr)
{
}

uint32_t DAS_STD_CALL PluginManagerServiceImpl::AddRef()
{
    return ++ref_count_;
}

uint32_t DAS_STD_CALL PluginManagerServiceImpl::Release()
{
    auto count = --ref_count_;
    if (count == 0)
    {
        delete this;
    }
    return count;
}

DasResult DAS_STD_CALL
PluginManagerServiceImpl::QueryInterface(const DasGuid& iid, void** pp_out)
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

    if (iid == DasIidOf<IDasPluginManagerService>())
    {
        *pp_out = static_cast<IDasPluginManagerService*>(this);
        AddRef();
        return DAS_S_OK;
    }

    *pp_out = nullptr;
    return DAS_E_NO_INTERFACE;
}

ComponentFactoryManager& PluginManagerServiceImpl::GetComponentFactoryManager()
{
    return mgr_.GetComponentFactoryManager();
}

std::span<FeatureInfo* const> PluginManagerServiceImpl::GetFeaturesByType(
    Das::PluginInterface::DasPluginFeature type) const
{
    return mgr_.GetFeaturesByType(type);
}

PluginPackageDesc* PluginManagerServiceImpl::FindPluginPackageByGuid(
    const DasGuid& guid)
{
    return mgr_.FindPluginPackageByGuid(guid);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

DAS_C_API DasResult CreateDasPluginManagerService(
    Das::Core::ForeignInterfaceHost::PluginManager& mgr,
    IDasPluginManagerService**                      pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* impl =
            new Das::Core::ForeignInterfaceHost::PluginManagerServiceImpl(mgr);
        impl->AddRef();
        *pp_out = impl;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}
