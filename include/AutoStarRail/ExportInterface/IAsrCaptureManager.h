#ifndef ASR_ASRCAPTUREMANAGER_H
#define ASR_ASRCAPTUREMANAGER_H

#include <AutoStarRail/IAsrBase.h>
#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/PluginInterface/IAsrCapture.h>

// {9ED8685E-050E-4FF5-9E6C-2A2C25CAC117}
ASR_DEFINE_GUID(
    ASR_IID_CAPTURE_MANAGER,
    IAsrCaptureManager,
    0x9ed8685e,
    0x50e,
    0x4ff5,
    0x9e,
    0x6c,
    0x2a,
    0x2c,
    0x25,
    0xca,
    0xc1,
    0x17);
SWIG_IGNORE(IAsrCaptureManager)
ASR_INTERFACE IAsrCaptureManager : public IAsrBase
{
    ASR_METHOD EnumCaptureLoadErrorState(
        const size_t         index,
        AsrResult*           p_error_code,
        IAsrReadOnlyString** pp_out_error_explanation) = 0;
    /**
     * @brief Enumerates all interfaces.
     * @param index
     * @param pp_out_interface
     * @return ASR_S_OK if interface is valid. Otherwise return error code which
     * is created by IAsrCaptureFactory.
     */
    ASR_METHOD EnumCaptureInterface(
        const size_t  index,
        IAsrCapture** pp_out_interface) = 0;
    ASR_METHOD RunCapturePerformanceTest() = 0;
    ASR_METHOD EnumCapturePerformanceTestResult(
        const size_t         index,
        AsrResult*           p_out_error_code,
        int32_t*             p_out_time_spent_in_ms,
        IAsrReadOnlyString** pp_out_error_explanation) = 0;
};

// {47556B91-FDC0-4AE7-B912-DC48AA917928}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_CAPTURE_MANAGER,
    IAsrSwigCaptureManager,
    0x47556b91,
    0xfdc0,
    0x4ae7,
    0xb9,
    0x12,
    0xdc,
    0x48,
    0xaa,
    0x91,
    0x79,
    0x28);
ASR_SWIG_EXPORT_ATTRIBUTE(IAsrSwigCaptureManager)
ASR_INTERFACE IAsrSwigCaptureManager : public IAsrSwigBase
{
    virtual AsrRetCapture EnumCaptureInterface(const size_t index) = 0;
};

ASR_DEFINE_RET_POINTER(AsrRetCaptureManager, IAsrSwigCaptureManager);

#endif // ASR_ASRCAPTUREMANAGER_H
