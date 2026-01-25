#ifndef DAS_CORE_FOREIGNINTERFACEHOST_IDASCAPTUREIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_IDASCAPTUREIMPL_H

#include "Das.PluginInterface.IDasCapture.hpp"
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasCaptureManager.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasCaptureManager.hpp>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class CaptureManagerImpl
    : public ExportInterface::DasCaptureManagerImplBase<CaptureManagerImpl>
{
public:
    struct ErrorInfo
    {
        DasPtr<IDasReadOnlyString> p_error_message;
        int32_t                    time_spent_in_ms;
        DasResult                  error_code;
    };

private:
    using ExpectedInstance =
        tl::expected<PluginInterface::DasCapture, ErrorInfo>;
    struct [[nodiscard("Do not acquire an instance and discard it.")]]
    CaptureInstance
    {
        DasReadOnlyString name;
        ExpectedInstance  instance;
    };

    DAS::Utils::RefCounter<CaptureManagerImpl> ref_counter_{};
    std::vector<CaptureInstance>               instances_{};
    std::vector<std::tuple<PluginInterface::DasCapture, ErrorInfo>>
        performance_results_{};

public:
    DAS_IMPL EnumLoadErrorState(
        uint64_t             index,
        DasResult*           p_error_code,
        IDasReadOnlyString** pp_out_error_explanation) override;
    DAS_IMPL EnumInterface(
        uint64_t                            index,
        DAS::PluginInterface::IDasCapture** pp_out_interface) override;
    DAS_IMPL RunPerformanceTest() override;
    // TODO: EnumCapturePerformanceTestResult 重构到 EnumPerformanceTestResult
    DAS_IMPL EnumCapturePerformanceTestResult(
        uint64_t                       index,
        DasResult*                     p_out_error_code,
        int32_t*                       p_out_time_spent_in_ms,
        PluginInterface::IDasCapture** pp_out_capture,
        IDasReadOnlyString**           pp_out_error_explanation);
    DAS_IMPL EnumPerformanceTestResult(
        uint64_t                            index,
        DasResult*                          p_out_error_code,
        int32_t*                            p_out_time_spent_in_ms,
        Das::PluginInterface::IDasCapture** pp_out_capture,
        IDasReadOnlyString**                pp_out_error_explanation) override;
    // impl
    void AddInstance(
        DasPtr<IDasReadOnlyString>           p_name,
        DasPtr<PluginInterface::IDasCapture> p_instance);
    void AddInstance(
        DasPtr<IDasReadOnlyString> p_name,
        const ErrorInfo&           error_info);
    /**
     * @brief Create a new capture instance with empty name.
     * @param error_info error code when getting the instance name.
     */
    void AddInstance(const ErrorInfo& error_info);
    void ReserveInstanceContainer(const std::size_t instance_count);
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_IDASCAPTUREIMPL_H
