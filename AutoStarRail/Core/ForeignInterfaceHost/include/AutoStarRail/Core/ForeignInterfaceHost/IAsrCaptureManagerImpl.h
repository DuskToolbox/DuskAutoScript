#ifndef ASR_CORE_FOREIGNINTERFACEHOST_IASRCAPTUREIMPL_H
#define ASR_CORE_FOREIGNINTERFACEHOST_IASRCAPTUREIMPL_H

#include <AutoStarRail/Core/ForeignInterfaceHost/Config.h>
#include <AutoStarRail/ExportInterface/IAsrCaptureManager.h>
#include <AutoStarRail/Utils/CommonUtils.hpp>
#include <AutoStarRail/Utils/Expected.h>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class CaptureManagerImpl;

class IAsrCaptureManagerImpl final : public IAsrCaptureManager
{
    CaptureManagerImpl& impl_;

public:
    IAsrCaptureManagerImpl(CaptureManagerImpl& impl);
    // IAsrBase
    int64_t  AddRef() override;
    int64_t  Release() override;
    ASR_IMPL QueryInterface(const AsrGuid& iid, void** pp_out_object) override;
    // IAsrCaptureManager
    ASR_IMPL EnumCaptureLoadErrorState(
        const size_t         index,
        AsrResult*           p_error_code,
        IAsrReadOnlyString** pp_out_error_explanation) override;
    ASR_IMPL EnumCaptureInterface(
        const size_t  index,
        IAsrCapture** pp_out_interface) override;
    ASR_IMPL RunCapturePerformanceTest() override;
    ASR_IMPL EnumCapturePerformanceTestResult(
        const size_t         index,
        AsrResult*           p_out_error_code,
        int32_t*             p_out_time_spent_in_ms,
        IAsrReadOnlyString** pp_out_error_explanation) override;
};

class IAsrSwigCaptureManagerImpl final : public IAsrSwigCaptureManager
{
    CaptureManagerImpl& impl_;

public:
    IAsrSwigCaptureManagerImpl(CaptureManagerImpl& impl);
    // IAsrSwigBase
    int64_t        AddRef() override;
    int64_t        Release() override;
    AsrRetSwigBase QueryInterface(const AsrGuid& iid) override;
    // IAsrSwigCaptureManager
    virtual AsrRetCapture EnumCaptureInterface(const size_t index) override;
};

class CaptureManagerImpl
{
public:
    struct ErrorInfo
    {
        AsrPtr<IAsrReadOnlyString> p_error_message;
        int32_t                    time_spent_in_ms;
        AsrResult                  error_code;
    };

private:
    using ExpectedInstance = tl::expected<AsrPtr<IAsrCapture>, ErrorInfo>;
    struct [[nodiscard(
        "Do not acquire an instance and discard it.")]] CaptureInstance
    {
        AsrReadOnlyString name;
        ExpectedInstance  instance;
    };

    ASR::Utils::RefCounter<CaptureManagerImpl> ref_counter_{};
    std::vector<CaptureInstance>               instances_{};
    std::vector<ErrorInfo>                     performance_results_{};
    IAsrCaptureManagerImpl                     cpp_projection_{*this};
    IAsrSwigCaptureManagerImpl                 swig_projection_{*this};

public:
    int64_t AddRef();
    int64_t Release();

    AsrResult EnumCaptureLoadErrorState(
        const size_t         index,
        AsrResult*           p_out_error_code,
        IAsrReadOnlyString** pp_out_error_explanation);
    AsrResult EnumCaptureInterface(
        const size_t  index,
        IAsrCapture** pp_out_interface);
    AsrResult RunCapturePerformanceTest();
    AsrResult EnumCapturePerformanceTestResult(
        const size_t         index,
        AsrResult*           p_out_error_code,
        int32_t*             p_out_time_spent_in_ms,
        IAsrReadOnlyString** pp_out_error_explanation);
    // impl
    void AddInstance(
        AsrPtr<IAsrReadOnlyString> p_name,
        AsrPtr<IAsrCapture>        p_instance);
    void AddInstance(
        AsrPtr<IAsrReadOnlyString> p_name,
        const ErrorInfo&           error_info);
    /**
     * @brief Create a new capture instance with empty name.
     * @param error_info error code when getting the instance name.
     */
    void AddInstance(const ErrorInfo& error_info);
    void ReserveInstanceContainer(const std::size_t instance_count);

    explicit operator IAsrCaptureManager*() noexcept;
    explicit operator IAsrSwigCaptureManager*() noexcept;
};

auto CreateAsrCaptureManagerImpl(IAsrReadOnlyString* p_json_config)
    -> std::pair<
        AsrResult,
        ASR::AsrPtr<ASR::Core::ForeignInterfaceHost::CaptureManagerImpl>>;

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // ASR_CORE_FOREIGNINTERFACEHOST_IASRCAPTUREIMPL_H
