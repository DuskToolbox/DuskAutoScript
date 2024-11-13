#include <AutoStarRail/AsrConfig.h>
#include <AutoStarRail/AsrPtr.hpp>
#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/Core/ForeignInterfaceHost/IAsrCaptureManagerImpl.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/PluginManager.h>
#include <AutoStarRail/Core/Utils/InternalUtils.h>
#include <AutoStarRail/Core/i18n/AsrResultTranslator.h>
#include <AutoStarRail/ExportInterface/IAsrCaptureManager.h>
#include <AutoStarRail/PluginInterface/IAsrCapture.h>
#include <AutoStarRail/PluginInterface/IAsrErrorLens.h>
#include <AutoStarRail/Utils/QueryInterface.hpp>
#include <AutoStarRail/Utils/StringUtils.h>
#include <AutoStarRail/Utils/Timer.hpp>
#include <utility>
#include <vector>

AsrResult AsrRetCaptureManagerLoadErrorState::GetErrorCode() noexcept
{
    return error_code;
}

AsrResult AsrRetCaptureManagerLoadErrorState::GetLoadResult() noexcept
{
    return load_result;
}

AsrReadOnlyString AsrRetCaptureManagerLoadErrorState::GetErrorMessage()
{
    return error_message;
}

AsrRetCaptureManagerPerformanceTestResult::
    AsrRetCaptureManagerPerformanceTestResult(
        AsrResult         error_code,
        AsrResult         test_result,
        IAsrSwigCapture*  p_capture,
        int32_t           time_spent_in_ms,
        AsrReadOnlyString error_message)
    : error_code_{error_code}, test_result_{test_result}, p_capture_{p_capture},
      time_spent_in_ms_{time_spent_in_ms},
      error_message_{std::move(error_message)}
{
}

AsrResult AsrRetCaptureManagerPerformanceTestResult::GetErrorCode()
    const noexcept
{
    return error_code_;
}

AsrResult AsrRetCaptureManagerPerformanceTestResult::GetTestResult()
    const noexcept
{
    return test_result_;
}

IAsrSwigCapture*
AsrRetCaptureManagerPerformanceTestResult::GetCapture() noexcept
{
    return p_capture_.Get();
}

int32_t AsrRetCaptureManagerPerformanceTestResult::GetTimeSpentInMs()
    const noexcept
{
    return time_spent_in_ms_;
}

AsrReadOnlyString AsrRetCaptureManagerPerformanceTestResult::GetErrorMessage()
    const noexcept
{
    return error_message_;
}

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

ASR_NS_ANONYMOUS_DETAILS_BEGIN

template <class T>
auto MakeErrorInfo(AsrResult error_code, T* p_error_generator)
    -> CaptureManagerImpl::ErrorInfo
{
    CaptureManagerImpl::ErrorInfo result{};
    result.error_code = error_code;
    std::string              error_message{};
    AsrReadOnlyStringWrapper asr_error_message{};
    const auto name = Utils::GetRuntimeClassNameFrom(p_error_generator);
    if (const auto get_error_message_result = ::AsrGetErrorMessage(
            p_error_generator,
            error_code,
            asr_error_message.Put());
        ASR::IsOk(get_error_message_result))
    {
        const char* u8_error_message;
        asr_error_message.GetTo(u8_error_message);
        error_message = ASR::fmt::format(
            R"(Error happened when creating capture instance.
TypeName: {}.
Error code: {}.
Error explanation: "{}".)",
            name,
            result.error_code,
            u8_error_message);
        result.p_error_message = asr_error_message.Get();
    }
    else
    {
        error_message = ASR::fmt::format(
            R"(Error happened when creating capture instance.
TypeName: {}.
Error code: {}.
No error explanation found. Result: {}.)",
            name,
            result.error_code,
            get_error_message_result);
    }
    ASR_CORE_LOG_ERROR(error_message);
    return result;
}

void OnCreateCaptureInstanceFailed(
    ASR::Core::ForeignInterfaceHost::CaptureManagerImpl::ErrorInfo&
                                           in_error_info,
    const ASR::AsrPtr<IAsrReadOnlyString>& p_capture_factory_name,
    const ASR::AsrPtr<ASR::Core::ForeignInterfaceHost::CaptureManagerImpl>&
        p_capture_manager)
{
    in_error_info =
        MakeErrorInfo(in_error_info.error_code, p_capture_factory_name.Get());
    p_capture_manager->AddInstance(p_capture_factory_name, in_error_info);
}

ASR_NS_ANONYMOUS_DETAILS_END

IAsrCaptureManagerImpl::IAsrCaptureManagerImpl(CaptureManagerImpl& impl)
    : impl_{impl}
{
}

int64_t IAsrCaptureManagerImpl::AddRef() { return impl_.AddRef(); }

int64_t IAsrCaptureManagerImpl::Release() { return impl_.Release(); }

AsrResult IAsrCaptureManagerImpl::QueryInterface(
    const AsrGuid& iid,
    void**         pp_out_object)
{
    return ASR::Utils::QueryInterface<IAsrCaptureManager>(
        this,
        iid,
        pp_out_object);
}

ASR_IMPL IAsrCaptureManagerImpl::EnumLoadErrorState(
    const size_t         index,
    AsrResult*           p_error_code,
    IAsrReadOnlyString** pp_out_error_explanation)
{
    return impl_.EnumCaptureLoadErrorState(
        index,
        p_error_code,
        pp_out_error_explanation);
}

ASR_IMPL IAsrCaptureManagerImpl::EnumInterface(
    const size_t  index,
    IAsrCapture** pp_out_interface)
{
    return impl_.EnumCaptureInterface(index, pp_out_interface);
}

ASR_IMPL IAsrCaptureManagerImpl::RunPerformanceTest()
{
    return impl_.RunCapturePerformanceTest();
}

ASR_IMPL IAsrCaptureManagerImpl::EnumPerformanceTestResult(
    const size_t         index,
    AsrResult*           p_out_error_code,
    int32_t*             p_out_time_spent_in_ms,
    IAsrCapture**        pp_out_capture,
    IAsrReadOnlyString** pp_out_error_explanation)
{
    return impl_.EnumCapturePerformanceTestResult(
        index,
        p_out_error_code,
        p_out_time_spent_in_ms,
        pp_out_capture,
        pp_out_error_explanation);
}

IAsrSwigCaptureManagerImpl::IAsrSwigCaptureManagerImpl(CaptureManagerImpl& impl)
    : impl_{impl}
{
}

int64_t IAsrSwigCaptureManagerImpl::AddRef() { return impl_.AddRef(); }

int64_t IAsrSwigCaptureManagerImpl::Release() { return impl_.Release(); }

AsrRetSwigBase IAsrSwigCaptureManagerImpl::QueryInterface(const AsrGuid& iid)
{
    return Utils::QueryInterface<IAsrSwigCaptureManager>(this, iid);
}

AsrRetCapture IAsrSwigCaptureManagerImpl::EnumInterface(const size_t index)
{
    AsrRetCapture       result;
    AsrPtr<IAsrCapture> p_result;
    result.error_code = impl_.EnumCaptureInterface(index, p_result.Put());
    try
    {
        const auto p_swig_result =
            MakeAsrPtr<IAsrSwigCapture, CppToSwig<IAsrCapture>>(p_result.Get());
        result.value = p_swig_result.Get();
    }
    catch (const std::bad_alloc&)
    {
        ASR_CORE_LOG_ERROR("NOTE: catching std::bad_alloc...");
        if (IsFailed(result.error_code))
        {
            ASR_CORE_LOG_ERROR(
                "Failed to call EnumCaptureInterface. Error code = {}.",
                result.error_code);
        }
        result.error_code = ASR_E_OUT_OF_MEMORY;
    }
    return result;
}

AsrRetCaptureManagerLoadErrorState
IAsrSwigCaptureManagerImpl::EnumLoadErrorState(size_t index)
{
    AsrRetCaptureManagerLoadErrorState result{};
    AsrPtr<IAsrReadOnlyString>         p_error_message{};
    result.error_code = impl_.EnumCaptureLoadErrorState(
        index,
        &result.load_result,
        p_error_message.Put());
    result.error_message = std::move(p_error_message);
    return result;
}

AsrResult IAsrSwigCaptureManagerImpl::RunPerformanceTest()
{
    return impl_.RunCapturePerformanceTest();
}

AsrRetCaptureManagerPerformanceTestResult
IAsrSwigCaptureManagerImpl::EnumPerformanceTestResult(size_t index)
{
    AsrResult                  error_code{ASR_E_UNDEFINED_RETURN_VALUE};
    AsrResult                  test_result{ASR_E_UNDEFINED_RETURN_VALUE};
    AsrPtr<IAsrCapture>        p_capture{};
    int32_t                    time_spent_in_ms{0};
    AsrPtr<IAsrReadOnlyString> error_message{};

    error_code = impl_.EnumCapturePerformanceTestResult(
        index,
        &test_result,
        &time_spent_in_ms,
        p_capture.Put(),
        error_message.Put());
    if (IsFailed(error_code))
    {
        return {error_code, test_result, nullptr, 0, {}};
    }

    const auto expected_p_swig_capture =
        MakeInterop<IAsrSwigCapture, IAsrCapture>(p_capture);
    if (!expected_p_swig_capture)
    {
        ASR_CORE_LOG_ERROR("Can not convert IAsrCapture to IAsrSwigCapture.");
        return {expected_p_swig_capture.error(), test_result, nullptr, 0, {}};
    }
    return {
        error_code,
        test_result,
        expected_p_swig_capture.value().Get(),
        time_spent_in_ms,
        {error_message.Get()}};
}

int64_t CaptureManagerImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t CaptureManagerImpl::Release() { return ref_counter_.Release(this); }

AsrResult CaptureManagerImpl::EnumCaptureLoadErrorState(
    const size_t         index,
    AsrResult*           p_out_error_code,
    IAsrReadOnlyString** pp_out_error_explanation)
{
    const auto size = instances_.size();
    if (index >= size)
    {
        return ASR_E_OUT_OF_RANGE;
    }
    const auto& instance = instances_[index];
    instance.instance
        .or_else(
            [p_out_error_code, pp_out_error_explanation](const auto& error_info)
            {
                if (p_out_error_code)
                {
                    *p_out_error_code = error_info.error_code;
                }
                if (pp_out_error_explanation)
                {
                    error_info.p_error_message->AddRef();
                    *pp_out_error_explanation =
                        error_info.p_error_message.Get();
                }
            })
        .map(
            [p_out_error_code, pp_out_error_explanation](const auto&)
            {
                *p_out_error_code = ASR_S_OK;
                ::CreateNullAsrString(pp_out_error_explanation);
            });
    return ASR_S_OK;
}

AsrResult CaptureManagerImpl::EnumCaptureInterface(
    const size_t  index,
    IAsrCapture** pp_out_interface)
{
    AsrResult result{ASR_E_UNDEFINED_RETURN_VALUE};
    if (index >= instances_.size())
    {
        *pp_out_interface = nullptr;
        return ASR_E_OUT_OF_RANGE;
    }
    const auto& instance = instances_[index];
    instance.instance
        .or_else(
            [pp_out_interface, &result](const auto& error_info)
            {
                *pp_out_interface = nullptr;
                result = error_info.error_code;
            })
        .map(
            [pp_out_interface, &result](const auto& capture)
            {
                capture->AddRef();
                *pp_out_interface = capture.Get();
                result = ASR_S_OK;
            });
    return result;
}

AsrResult CaptureManagerImpl::RunCapturePerformanceTest()
{
    AsrResult result{ASR_S_OK};
    performance_results_.clear();
    performance_results_.reserve(instances_.size());
    for (const auto& [_, instance] : instances_)
    {
        if (!instance)
        {
            continue;
        }
        AsrPtr<IAsrImage> p_image{};
        ErrorInfo         capture_error_info{};
        const auto        p_capture = instance.value();
        ASR::Utils::Timer timer{};
        timer.Begin();
        const auto capture_result = p_capture->Capture(p_image.Put());
        if (IsFailed(capture_result))
        {
            result = ASR_S_FALSE;
            capture_error_info =
                Details::MakeErrorInfo(capture_result, p_capture.Get());
            performance_results_.emplace_back(p_capture, capture_error_info);

            continue;
        }
        capture_error_info.time_spent_in_ms =
            static_cast<decltype(capture_error_info.time_spent_in_ms)>(
                timer.End());
        capture_error_info.error_code = capture_result;
        ::CreateNullAsrString(capture_error_info.p_error_message.Put());
        performance_results_.emplace_back(p_capture, capture_error_info);
    }
    // 实现调度队列后再实现这个函数
    return result;
}

AsrResult CaptureManagerImpl::EnumCapturePerformanceTestResult(
    const size_t         index,
    AsrResult*           p_out_error_code,
    int32_t*             p_out_time_spent_in_ms,
    IAsrCapture**        pp_out_capture,
    IAsrReadOnlyString** pp_out_error_explanation)
{
    if (index == instances_.size())
    {
        return ASR_E_OUT_OF_RANGE;
    }
    try
    {
        auto& [object, error_info] = performance_results_.at(index);

        if (p_out_error_code)
        {
            *p_out_error_code = error_info.error_code;
        }
        if (p_out_time_spent_in_ms)
        {
            *p_out_time_spent_in_ms = error_info.time_spent_in_ms;
        }
        if (pp_out_capture)
        {
            *pp_out_capture = object.Get();
            (*pp_out_capture)->AddRef();
        }
        if (pp_out_error_explanation)
        {
            *pp_out_error_explanation = error_info.p_error_message.Get();
            (*pp_out_error_explanation)->AddRef();
        }
        return ASR_S_OK;
    }
    catch (const std::out_of_range& ex)
    {
        ASR_CORE_LOG_ERROR(
            "Index out of range when calling EnumCapturePerformanceTestResult. The error info size is {}. Input index is {}. Message: \"{}\".",
            performance_results_.size(),
            index,
            ex.what());
        return ASR_E_OUT_OF_RANGE;
    }
}

void CaptureManagerImpl::ReserveInstanceContainer(
    const std::size_t instance_count)
{
    instances_.reserve(instance_count);
}

void CaptureManagerImpl::AddInstance(
    AsrPtr<IAsrReadOnlyString> p_name,
    AsrPtr<IAsrCapture>        p_instance)
{
    instances_.emplace_back(AsrReadOnlyString{std::move(p_name)}, p_instance);
}

void CaptureManagerImpl::AddInstance(
    AsrPtr<IAsrReadOnlyString>           p_name,
    const CaptureManagerImpl::ErrorInfo& error_info)
{
    instances_.emplace_back(
        AsrReadOnlyString{std::move(p_name)},
        tl::make_unexpected(error_info));
}

void CaptureManagerImpl::AddInstance(
    const CaptureManagerImpl::ErrorInfo& error_info)
{
    instances_.emplace_back(
        AsrReadOnlyString{},
        tl::make_unexpected(error_info));
}

CaptureManagerImpl::operator IAsrCaptureManager*() noexcept
{
    return &cpp_projection_;
}

CaptureManagerImpl::operator IAsrSwigCaptureManager*() noexcept
{
    return &swig_projection_;
}

auto CreateAsrCaptureManagerImpl(
    const std::vector<AsrPtr<IAsrCaptureFactory>>& capture_factories,
    IAsrReadOnlyString*                            p_environment_json_config,
    PluginManager&                                 plugin_manager)
    -> std::pair<
        AsrResult,
        ASR::AsrPtr<ASR::Core::ForeignInterfaceHost::CaptureManagerImpl>>
{
    ASR::AsrPtr<IAsrReadOnlyString> p_locale_name{};
    ASR::AsrPtr<ASR::Core::ForeignInterfaceHost::CaptureManagerImpl>
              p_capture_manager{};
    AsrResult result{ASR_S_OK};

    try
    {
        p_capture_manager = ASR::MakeAsrPtr<
            ASR::Core::ForeignInterfaceHost::CaptureManagerImpl>();
    }
    catch (std::bad_alloc&)
    {
        ASR_CORE_LOG_ERROR("Out of memory!");
        return {ASR_E_OUT_OF_MEMORY, nullptr};
    }

    ::AsrGetDefaultLocale(p_locale_name.Put());
    p_capture_manager->ReserveInstanceContainer(capture_factories.size());
    for (const auto& p_factory : capture_factories)
    {
        ASR::Core::ForeignInterfaceHost::CaptureManagerImpl::ErrorInfo
                                        error_info{};
        ASR::AsrPtr<IAsrReadOnlyString> capture_factory_name;
        AsrGuid                         factory_iid{};
        try
        {
            capture_factory_name =
                ASR::Core::Utils::GetRuntimeClassNameFrom(p_factory.Get());
            factory_iid = Utils::GetGuidFrom(p_factory.Get());
        }
        catch (const ASR::Core::AsrException& ex)
        {
            ASR_CORE_LOG_ERROR(
                "Can not resolve capture factory type name or iid.");
            ASR_CORE_LOG_EXCEPTION(ex);
            result = ASR_FALSE;
            error_info.error_code = ex.GetErrorCode();
            continue;
        }

        ASR::AsrPtr<IAsrCapture> p_instance{};
        const auto               opt_ref_interface_static_storage =
            plugin_manager.FindInterfaceStaticStorage(factory_iid);
        if (!opt_ref_interface_static_storage)
        {
            ASR_CORE_LOG_ERROR(
                "No matched interface storage! Iid = {}.",
                factory_iid);
            result = ASR_FALSE;
            error_info.error_code = opt_ref_interface_static_storage.error();
            continue;
        }
        AsrPtr<IAsrReadOnlyString> p_plugin_config{};
        opt_ref_interface_static_storage.value()
            .get()
            .sp_desc->settings_json_->GetValue(p_plugin_config.Put());
        if (const auto error_code = p_factory->CreateInstance(
                p_environment_json_config,
                p_plugin_config.Get(),
                p_instance.Put());
            ASR::IsFailed(error_code))
        {
            result = ASR_S_FALSE;
            error_info.error_code = error_code;
            // 补个接口
            Details::OnCreateCaptureInstanceFailed(
                error_info,
                capture_factory_name,
                p_capture_manager);
            continue;
        }

        try
        {
            const auto capture_name =
                ASR::Core::Utils::GetRuntimeClassNameFrom(p_instance.Get());
            p_capture_manager->AddInstance(capture_name, p_instance);
        }
        catch (const ASR::Core::AsrException& ex)
        {
            ASR_CORE_LOG_ERROR("Get IAsrCapture object name failed.");
            ASR_CORE_LOG_EXCEPTION(ex);
            result = ASR_FALSE;
            ASR::AsrPtr<IAsrReadOnlyString> p_null_string{};
            ::CreateNullAsrString(p_null_string.Put());
            p_capture_manager->AddInstance(p_null_string, p_instance);
        }
    }

    return {result, p_capture_manager};
}

ASR_CORE_FOREIGNINTERFACEHOST_NS_END
