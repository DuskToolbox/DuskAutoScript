#include "AdbTouchFactoryImpl.h"
#include <array>
#include <das/DasApi.h>
#include <stdexcept>

#define DAS_BUILD_SHARED
#include "AdbTouchFactoryImpl.h"
#include "PluginImpl.h"

DAS_NS_BEGIN

DAS_IMPL DasAdbTouchPlugin::QueryInterface(const DasGuid& iid, void** pp_object)
{
    if (pp_object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 检查IID_IDasPluginPackage
    if (iid == DasIidOf<PluginInterface::IDasPluginPackage>())
    {
        *pp_object = static_cast<PluginInterface::IDasPluginPackage*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // 检查IID_IDasBase
    if (iid == DAS_IID_BASE)
    {
        *pp_object = static_cast<IDasBase*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    *pp_object = nullptr;
    return DAS_E_NO_INTERFACE;
}

DAS_IMPL DasAdbTouchPlugin::EnumFeature(
    size_t            index,
    DasPluginFeature* p_out_feature)
{
    if (p_out_feature == nullptr)
    {
        DAS_LOG_ERROR("Nullptr found.");
        return DAS_E_INVALID_POINTER;
    }

    std::array features{Das::PluginInterface::DAS_PLUGIN_FEATURE_INPUT_FACTORY};

    try
    {
        const auto value = features.at(index);
        *p_out_feature = value;
        return DAS_S_OK;
    }
    catch (const std::out_of_range&)
    {
        return DAS_E_OUT_OF_RANGE;
    }
}

DAS_IMPL DasAdbTouchPlugin::CreateFeatureInterface(
    uint64_t index,
    void**   pp_out_interface)
{
    if (pp_out_interface == nullptr)
    {
        DAS_LOG_ERROR("Nullptr found.");
        return DAS_E_INVALID_POINTER;
    }

    if (index == 0)
    {
        try
        {
            const auto p_factory =
                MakeDasPtr<IDasInputFactory, AdbTouchFactory>();
            *pp_out_interface = p_factory.Get();
            p_factory->AddRef();
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }
    return DAS_E_OUT_OF_RANGE;
}

DAS_BOOL_IMPL DasAdbTouchPlugin::CanUnloadNow()
{
    return DAS_E_NO_IMPLEMENTATION;
}

DAS_NS_END
