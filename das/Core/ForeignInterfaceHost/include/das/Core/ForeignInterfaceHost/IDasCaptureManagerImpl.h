#ifndef DAS_CORE_FOREIGNINTERFACEHOST_IDASCAPTUREIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_IDASCAPTUREIMPL_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/ExportInterface/IDasCaptureManager.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class CaptureManagerImpl;

class IDasCaptureManagerImpl final : public IDasCaptureManager
{
    CaptureManagerImpl& impl_;

public:
    IDasCaptureManagerImpl(CaptureManagerImpl& impl);
    // IDasBase
    int64_t  AddRef() override;
    int64_t  Release() override;
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_out_object) override;
    // IDasCaptureManager
    DAS_IMPL EnumLoadErrorState(
        const size_t         index,
        DasResult*           p_error_code,
        IDasReadOnlyString** pp_out_error_explanation) override;
    DAS_IMPL EnumInterface(const size_t index, IDasCapture** pp_out_interface)
        override;
    DAS_IMPL RunPerformanceTest() override;
    DAS_IMPL EnumPerformanceTestResult(
        const size_t         index,
        DasResult*           p_out_error_code,
        int32_t*             p_out_time_spent_in_ms,
        IDasCapture**        pp_out_capture,
        IDasReadOnlyString** pp_out_error_explanation) override;
};

class IDasSwigCaptureManagerImpl final : public IDasSwigCaptureManager
{
    CaptureManagerImpl& impl_;

public:
    IDasSwigCaptureManagerImpl(CaptureManagerImpl& impl);
    // IDasSwigBase
    int64_t        AddRef() override;
    int64_t        Release() override;
    DasRetSwigBase QueryInterface(const DasGuid& iid) override;
    // IDasSwigCaptureManager
    DasRetCapture EnumInterface(const size_t index) override;
    DasRetCaptureManagerLoadErrorState EnumLoadErrorState(
        size_t index) override;
    DasResult                                 RunPerformanceTest() override;
    DasRetCaptureManagerPerformanceTestResult EnumPerformanceTestResult(
        size_t index) override;
};

class CaptureManagerImpl
{
public:
    struct ErrorInfo
    {
        DasPtr<IDasReadOnlyString> p_error_message;
        int32_t                    time_spent_in_ms;
        DasResult                  error_code;
    };

private:
    using ExpectedInstance = tl::expected<DasPtr<IDasCapture>, ErrorInfo>;
    struct [[nodiscard(
        "Do not acquire an instance and discard it.")]] CaptureInstance
    {
        DasReadOnlyString name;
        ExpectedInstance  instance;
    };

    DAS::Utils::RefCounter<CaptureManagerImpl> ref_counter_{};
    std::vector<CaptureInstance>               instances_{};
    std::vector<std::tuple<DasPtr<IDasCapture>, ErrorInfo>>
                               performance_results_{};
    IDasCaptureManagerImpl     cpp_projection_{*this};
    IDasSwigCaptureManagerImpl swig_projection_{*this};

public:
    int64_t AddRef();
    int64_t Release();

    DasResult EnumCaptureLoadErrorState(
        const size_t         index,
        DasResult*           p_out_error_code,
        IDasReadOnlyString** pp_out_error_explanation);
    DasResult EnumCaptureInterface(
        const size_t  index,
        IDasCapture** pp_out_interface);
    DasResult RunCapturePerformanceTest();
    DasResult EnumCapturePerformanceTestResult(
        const size_t         index,
        DasResult*           p_out_error_code,
        int32_t*             p_out_time_spent_in_ms,
        IDasCapture**        pp_out_capture,
        IDasReadOnlyString** pp_out_error_explanation);
    // impl
    void AddInstance(
        DasPtr<IDasReadOnlyString> p_name,
        DasPtr<IDasCapture>        p_instance);
    void AddInstance(
        DasPtr<IDasReadOnlyString> p_name,
        const ErrorInfo&           error_info);
    /**
     * @brief Create a new capture instance with empty name.
     * @param error_info error code when getting the instance name.
     */
    void AddInstance(const ErrorInfo& error_info);
    void ReserveInstanceContainer(const std::size_t instance_count);

    explicit operator IDasCaptureManager*() noexcept;
    explicit operator IDasSwigCaptureManager*() noexcept;
};

auto CreateDasCaptureManagerImpl(
    const std::vector<DasPtr<IDasCaptureFactory>>& capture_factories,
    IDasReadOnlyString*                            p_environment_json_config,
    PluginManager&                                 plugin_manager)
    -> std::pair<
        DasResult,
        DAS::DasPtr<DAS::Core::ForeignInterfaceHost::CaptureManagerImpl>>;

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_IDASCAPTUREIMPL_H
