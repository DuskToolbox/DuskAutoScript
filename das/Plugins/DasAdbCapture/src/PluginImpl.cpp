#include <das/ExportInterface/DasLogger.h>
#include <das/IDasBase.h>
#include <das/PluginInterface/IDasPluginPackage.h>
#include <das/Utils/QueryInterface.hpp>
#include <das/Utils/StringUtils.h>

#define DAS_BUILD_SHARED

#include "AdbCaptureFactoryImpl.h"
#include "PluginImpl.h"

#include <array>
#include <stdexcept>

DAS_NS_BEGIN

int64_t AdbCapturePlugin::AddRef() { return ref_counter_.AddRef(); }

int64_t AdbCapturePlugin::Release() { return ref_counter_.Release(this); }

DasResult AdbCapturePlugin::QueryInterface(
    const DasGuid& iid,
    void**         pp_out_object)
{
    return DAS::Utils::QueryInterface<IDasPluginPackage>(
        this,
        iid,
        pp_out_object);
}

DasResult AdbCapturePlugin::EnumFeature(
    const size_t      index,
    DasPluginFeature* p_out_feature)
{
    static std::array features{
        DAS_PLUGIN_FEATURE_CAPTURE_FACTORY,
        DAS_PLUGIN_FEATURE_ERROR_LENS};
    try
    {
        const auto result = features.at(index);
        *p_out_feature = result;
        return result;
    }
    catch (const std::out_of_range& ex)
    {
        DAS_LOG_ERROR(ex.what());
        return DAS_E_OUT_OF_RANGE;
    }
}

DasResult AdbCapturePlugin::CreateFeatureInterface(
    size_t index,
    void** pp_out_interface)
{
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_interface);
    switch (index)
    {
        // Capture Factory
    case 0:
    {
        const auto p_result =
            MakeDasPtr<IDasCaptureFactory, AdbCaptureFactoryImpl>();
        *pp_out_interface = p_result.Get();
        p_result->AddRef();
        return DAS_S_OK;
    }
        // Error lens 暂时用不到，先不启用
    case 1:
        [[fallthrough]];
    default:
        *pp_out_interface = nullptr;
        return DAS_E_OUT_OF_RANGE;
    }
}

static std::atomic_int32_t g_ref_count;

DasResult AdbCapturePlugin::CanUnloadNow()
{
    return g_ref_count == 0 ? DAS_TRUE : DAS_FALSE;
}

void AdbCaptureAddRef() { g_ref_count++; }

void AdbCaptureRelease() { g_ref_count--; }

DAS_NS_END
