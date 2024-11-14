#ifndef DAS_DASCAPTUREMANAGER_H
#define DAS_DASCAPTUREMANAGER_H

#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/PluginInterface/IDasCapture.h>

// {9ED8685E-050E-4FF5-9E6C-2A2C25CAC117}
DAS_DEFINE_GUID(
    DAS_IID_CAPTURE_MANAGER,
    IDasCaptureManager,
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
SWIG_IGNORE(IDasCaptureManager)
DAS_INTERFACE IDasCaptureManager : public IDasBase
{
    DAS_METHOD EnumLoadErrorState(
        const size_t         index,
        DasResult*           p_error_code,
        IDasReadOnlyString** pp_out_error_explanation) = 0;
    /**
     * @brief Enumerates all interfaces.
     * @param index
     * @param pp_out_interface
     * @return DAS_S_OK if interface is valid. Otherwise return error code which
     * is created by IDasCaptureFactory.
     */
    DAS_METHOD EnumInterface(
        const size_t  index,
        IDasCapture** pp_out_interface) = 0;
    DAS_METHOD RunPerformanceTest() = 0;
    DAS_METHOD EnumPerformanceTestResult(
        const size_t         index,
        DasResult*           p_out_error_code,
        int32_t*             p_out_time_spent_in_ms,
        IDasCapture**        pp_out_capture,
        IDasReadOnlyString** pp_out_error_explanation) = 0;
};

struct DasRetCaptureManagerLoadErrorState
{
    SWIG_PRIVATE
    DasResult         error_code{DAS_E_UNDEFINED_RETURN_VALUE};
    DasResult         load_result{DAS_E_UNDEFINED_RETURN_VALUE};
    DasReadOnlyString error_message{};

public:
    DAS_EXPORT DasResult         GetErrorCode() noexcept;
    DAS_EXPORT DasResult         GetLoadResult() noexcept;
    DAS_EXPORT DasReadOnlyString GetErrorMessage();
};

class DasRetCaptureManagerPerformanceTestResult
{
    DasResult                    error_code_{DAS_E_UNDEFINED_RETURN_VALUE};
    DasResult                    test_result_{DAS_E_UNDEFINED_RETURN_VALUE};
    DAS::DasPtr<IDasSwigCapture> p_capture_{};
    int32_t                      time_spent_in_ms_{0};
    DasReadOnlyString            error_message_{};

public:
    SWIG_PRIVATE
    DasRetCaptureManagerPerformanceTestResult(
        DasResult         error_code,
        DasResult         test_result,
        IDasSwigCapture*  p_capture,
        int32_t           time_spent_in_ms,
        DasReadOnlyString error_message);

public:
    DasRetCaptureManagerPerformanceTestResult() = default;
    DAS_EXPORT DasResult         GetErrorCode() const noexcept;
    DAS_EXPORT DasResult         GetTestResult() const noexcept;
    DAS_EXPORT IDasSwigCapture*  GetCapture() noexcept;
    DAS_EXPORT int32_t           GetTimeSpentInMs() const noexcept;
    DAS_EXPORT DasReadOnlyString GetErrorMessage() const noexcept;
};

// {47556B91-FDC0-4AE7-B912-DC48AA917928}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_CAPTURE_MANAGER,
    IDasSwigCaptureManager,
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
DAS_SWIG_EXPORT_ATTRIBUTE(IDasSwigCaptureManager)
DAS_INTERFACE IDasSwigCaptureManager : public IDasSwigBase
{
    virtual DasRetCaptureManagerLoadErrorState EnumLoadErrorState(
        size_t index) = 0;
    virtual DasRetCapture EnumInterface(const size_t index) = 0;
    virtual DasResult     RunPerformanceTest() = 0;
    virtual DasRetCaptureManagerPerformanceTestResult EnumPerformanceTestResult(
        size_t index) = 0;
};

DAS_DEFINE_RET_POINTER(DasRetCaptureManager, IDasSwigCaptureManager);

#endif // DAS_DASCAPTUREMANAGER_H
