#include "AdbTouchFactoryImpl.h"
#include <array>
#include <das/ExportInterface/DasLogger.h>
#include <das/Utils/QueryInterface.hpp>
#include <stdexcept>

#define DAS_BUILD_SHARED
#include "AdbTouchFactoryImpl.h"
#include "PluginImpl.h"

DAS_NS_BEGIN

DAS_IMPL DasAdbTouchPlugin::QueryInterface(const DasGuid& iid, void** pp_object)
{
    return Utils::QueryInterface<IDasPluginPackage>(this, iid, pp_object);
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

    std::array features{DAS_PLUGIN_FEATURE_INPUT_FACTORY};

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
    size_t index,
    void** pp_out_interface)
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
