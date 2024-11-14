#include <das/DasConfig.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Core/ForeignInterfaceHost/IDasCaptureManagerImpl.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/Utils/InternalUtils.h>
#include <das/Core/i18n/DasResultTranslator.h>
#include <das/ExportInterface/IDasCaptureManager.h>
#include <das/PluginInterface/IDasCapture.h>
#include <das/PluginInterface/IDasErrorLens.h>
#include <das/Utils/QueryInterface.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/Timer.hpp>
#include <utility>
#include <vector>

DasResult DasRetCaptureManagerLoadErrorState::GetErrorCode() noexcept
{
    return error_code;
}

DasResult DasRetCaptureManagerLoadErrorState::GetLoadResult() noexcept
{
    return load_result;
}

DasReadOnlyString DasRetCaptureManagerLoadErrorState::GetErrorMessage()
{
    return error_message;
}

DasRetCaptureManagerPerformanceTestResult::
    DasRetCaptureManagerPerformanceTestResult(
        DasResult         error_code,
        DasResult         test_result,
        IDasSwigCapture*  p_capture,
        int32_t           time_spent_in_ms,
        DasReadOnlyString error_message)
    : error_code_{error_code}, test_result_{test_result}, p_capture_{p_capture},
      time_spent_in_ms_{time_spent_in_ms},
      error_message_{std::move(error_message)}
{
}

DasResult DasRetCaptureManagerPerformanceTestResult::GetErrorCode()
    const noexcept
{
    return error_code_;
}

DasResult DasRetCaptureManagerPerformanceTestResult::GetTestResult()
    const noexcept
{
    return test_result_;
}

IDasSwigCapture*
DasRetCaptureManagerPerformanceTestResult::GetCapture() noexcept
{
    return p_capture_.Get();
}

int32_t DasRetCaptureManagerPerformanceTestResult::GetTimeSpentInMs()
    const noexcept
{
    return time_spent_in_ms_;
}

DasReadOnlyString DasRetCaptureManagerPerformanceTestResult::GetErrorMessage()
    const noexcept
{
    return error_message_;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

template <class T>
auto MakeErrorInfo(DasResult error_code, T* p_error_generator)
    -> CaptureManagerImpl::ErrorInfo
{
    CaptureManagerImpl::ErrorInfo result{};
    result.error_code = error_code;
    std::string              error_message{};
    DasReadOnlyStringWrapper asr_error_message{};
    const auto name = Utils::GetRuntimeClassNameFrom(p_error_generator);
    if (const auto get_error_message_result = ::DasGetErrorMessage(
            p_error_generator,
            error_code,
            asr_error_message.Put());
        DAS::IsOk(get_error_message_result))
    {
        const char* u8_error_message;
        asr_error_message.GetTo(u8_error_message);
        error_message = DAS::fmt::format(
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
        error_message = DAS::fmt::format(
            R"(Error happened when creating capture instance.
TypeName: {}.
Error code: {}.
No error explanation found. Result: {}.)",
            name,
            result.error_code,
            get_error_message_result);
    }
    DAS_CORE_LOG_ERROR(error_message);
    return result;
}

void OnCreateCaptureInstanceFailed(
    DAS::Core::ForeignInterfaceHost::CaptureManagerImpl::ErrorInfo&
                                           in_error_info,
    const DAS::DasPtr<IDasReadOnlyString>& p_capture_factory_name,
    const DAS::DasPtr<DAS::Core::ForeignInterfaceHost::CaptureManagerImpl>&
        p_capture_manager)
{
    in_error_info =
        MakeErrorInfo(in_error_info.error_code, p_capture_factory_name.Get());
    p_capture_manager->AddInstance(p_capture_factory_name, in_error_info);
}

DAS_NS_ANONYMOUS_DETAILS_END

IDasCaptureManagerImpl::IDasCaptureManagerImpl(CaptureManagerImpl& impl)
    : impl_{impl}
{
}

int64_t IDasCaptureManagerImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasCaptureManagerImpl::Release() { return impl_.Release(); }

DasResult IDasCaptureManagerImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_out_object)
{
    return DAS::Utils::QueryInterface<IDasCaptureManager>(
        this,
        iid,
        pp_out_object);
}

DAS_IMPL IDasCaptureManagerImpl::EnumLoadErrorState(
    const size_t         index,
    DasResult*           p_error_code,
    IDasReadOnlyString** pp_out_error_explanation)
{
    return impl_.EnumCaptureLoadErrorState(
        index,
        p_error_code,
        pp_out_error_explanation);
}

DAS_IMPL IDasCaptureManagerImpl::EnumInterface(
    const size_t  index,
    IDasCapture** pp_out_interface)
{
    return impl_.EnumCaptureInterface(index, pp_out_interface);
}

DAS_IMPL IDasCaptureManagerImpl::RunPerformanceTest()
{
    return impl_.RunCapturePerformanceTest();
}

DAS_IMPL IDasCaptureManagerImpl::EnumPerformanceTestResult(
    const size_t         index,
    DasResult*           p_out_error_code,
    int32_t*             p_out_time_spent_in_ms,
    IDasCapture**        pp_out_capture,
    IDasReadOnlyString** pp_out_error_explanation)
{
    return impl_.EnumCapturePerformanceTestResult(
        index,
        p_out_error_code,
        p_out_time_spent_in_ms,
        pp_out_capture,
        pp_out_error_explanation);
}

IDasSwigCaptureManagerImpl::IDasSwigCaptureManagerImpl(CaptureManagerImpl& impl)
    : impl_{impl}
{
}

int64_t IDasSwigCaptureManagerImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasSwigCaptureManagerImpl::Release() { return impl_.Release(); }

DasRetSwigBase IDasSwigCaptureManagerImpl::QueryInterface(const DasGuid& iid)
{
    return Utils::QueryInterface<IDasSwigCaptureManager>(this, iid);
}

DasRetCapture IDasSwigCaptureManagerImpl::EnumInterface(const size_t index)
{
    DasRetCapture       result;
    DasPtr<IDasCapture> p_result;
    result.error_code = impl_.EnumCaptureInterface(index, p_result.Put());
    try
    {
        const auto p_swig_result =
            MakeDasPtr<IDasSwigCapture, CppToSwig<IDasCapture>>(p_result.Get());
        result.value = p_swig_result.Get();
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("NOTE: catching std::bad_alloc...");
        if (IsFailed(result.error_code))
        {
            DAS_CORE_LOG_ERROR(
                "Failed to call EnumCaptureInterface. Error code = {}.",
                result.error_code);
        }
        result.error_code = DAS_E_OUT_OF_MEMORY;
    }
    return result;
}

DasRetCaptureManagerLoadErrorState
IDasSwigCaptureManagerImpl::EnumLoadErrorState(size_t index)
{
    DasRetCaptureManagerLoadErrorState result{};
    DasPtr<IDasReadOnlyString>         p_error_message{};
    result.error_code = impl_.EnumCaptureLoadErrorState(
        index,
        &result.load_result,
        p_error_message.Put());
    result.error_message = std::move(p_error_message);
    return result;
}

DasResult IDasSwigCaptureManagerImpl::RunPerformanceTest()
{
    return impl_.RunCapturePerformanceTest();
}

DasRetCaptureManagerPerformanceTestResult
IDasSwigCaptureManagerImpl::EnumPerformanceTestResult(size_t index)
{
    DasResult                  error_code{DAS_E_UNDEFINED_RETURN_VALUE};
    DasResult                  test_result{DAS_E_UNDEFINED_RETURN_VALUE};
    DasPtr<IDasCapture>        p_capture{};
    int32_t                    time_spent_in_ms{0};
    DasPtr<IDasReadOnlyString> error_message{};

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
        MakeInterop<IDasSwigCapture, IDasCapture>(p_capture);
    if (!expected_p_swig_capture)
    {
        DAS_CORE_LOG_ERROR("Can not convert IDasCapture to IDasSwigCapture.");
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

DasResult CaptureManagerImpl::EnumCaptureLoadErrorState(
    const size_t         index,
    DasResult*           p_out_error_code,
    IDasReadOnlyString** pp_out_error_explanation)
{
    const auto size = instances_.size();
    if (index >= size)
    {
        return DAS_E_OUT_OF_RANGE;
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
                *p_out_error_code = DAS_S_OK;
                ::CreateNullDasString(pp_out_error_explanation);
            });
    return DAS_S_OK;
}

DasResult CaptureManagerImpl::EnumCaptureInterface(
    const size_t  index,
    IDasCapture** pp_out_interface)
{
    DasResult result{DAS_E_UNDEFINED_RETURN_VALUE};
    if (index >= instances_.size())
    {
        *pp_out_interface = nullptr;
        return DAS_E_OUT_OF_RANGE;
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
                result = DAS_S_OK;
            });
    return result;
}

DasResult CaptureManagerImpl::RunCapturePerformanceTest()
{
    DasResult result{DAS_S_OK};
    performance_results_.clear();
    performance_results_.reserve(instances_.size());
    for (const auto& [_, instance] : instances_)
    {
        if (!instance)
        {
            continue;
        }
        DasPtr<IDasImage> p_image{};
        ErrorInfo         capture_error_info{};
        const auto        p_capture = instance.value();
        DAS::Utils::Timer timer{};
        timer.Begin();
        const auto capture_result = p_capture->Capture(p_image.Put());
        if (IsFailed(capture_result))
        {
            result = DAS_S_FALSE;
            capture_error_info =
                Details::MakeErrorInfo(capture_result, p_capture.Get());
            performance_results_.emplace_back(p_capture, capture_error_info);

            continue;
        }
        capture_error_info.time_spent_in_ms =
            static_cast<decltype(capture_error_info.time_spent_in_ms)>(
                timer.End());
        capture_error_info.error_code = capture_result;
        ::CreateNullDasString(capture_error_info.p_error_message.Put());
        performance_results_.emplace_back(p_capture, capture_error_info);
    }
    // 实现调度队列后再实现这个函数
    return result;
}

DasResult CaptureManagerImpl::EnumCapturePerformanceTestResult(
    const size_t         index,
    DasResult*           p_out_error_code,
    int32_t*             p_out_time_spent_in_ms,
    IDasCapture**        pp_out_capture,
    IDasReadOnlyString** pp_out_error_explanation)
{
    if (index == instances_.size())
    {
        return DAS_E_OUT_OF_RANGE;
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
        return DAS_S_OK;
    }
    catch (const std::out_of_range& ex)
    {
        DAS_CORE_LOG_ERROR(
            "Index out of range when calling EnumCapturePerformanceTestResult. The error info size is {}. Input index is {}. Message: \"{}\".",
            performance_results_.size(),
            index,
            ex.what());
        return DAS_E_OUT_OF_RANGE;
    }
}

void CaptureManagerImpl::ReserveInstanceContainer(
    const std::size_t instance_count)
{
    instances_.reserve(instance_count);
}

void CaptureManagerImpl::AddInstance(
    DasPtr<IDasReadOnlyString> p_name,
    DasPtr<IDasCapture>        p_instance)
{
    instances_.emplace_back(DasReadOnlyString{std::move(p_name)}, p_instance);
}

void CaptureManagerImpl::AddInstance(
    DasPtr<IDasReadOnlyString>           p_name,
    const CaptureManagerImpl::ErrorInfo& error_info)
{
    instances_.emplace_back(
        DasReadOnlyString{std::move(p_name)},
        tl::make_unexpected(error_info));
}

void CaptureManagerImpl::AddInstance(
    const CaptureManagerImpl::ErrorInfo& error_info)
{
    instances_.emplace_back(
        DasReadOnlyString{},
        tl::make_unexpected(error_info));
}

CaptureManagerImpl::operator IDasCaptureManager*() noexcept
{
    return &cpp_projection_;
}

CaptureManagerImpl::operator IDasSwigCaptureManager*() noexcept
{
    return &swig_projection_;
}

auto CreateDasCaptureManagerImpl(
    const std::vector<DasPtr<IDasCaptureFactory>>& capture_factories,
    IDasReadOnlyString*                            p_environment_json_config,
    PluginManager&                                 plugin_manager)
    -> std::pair<
        DasResult,
        DAS::DasPtr<DAS::Core::ForeignInterfaceHost::CaptureManagerImpl>>
{
    DAS::DasPtr<IDasReadOnlyString> p_locale_name{};
    DAS::DasPtr<DAS::Core::ForeignInterfaceHost::CaptureManagerImpl>
              p_capture_manager{};
    DasResult result{DAS_S_OK};

    try
    {
        p_capture_manager = DAS::MakeDasPtr<
            DAS::Core::ForeignInterfaceHost::CaptureManagerImpl>();
    }
    catch (std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("Out of memory!");
        return {DAS_E_OUT_OF_MEMORY, nullptr};
    }

    ::DasGetDefaultLocale(p_locale_name.Put());
    p_capture_manager->ReserveInstanceContainer(capture_factories.size());
    for (const auto& p_factory : capture_factories)
    {
        DAS::Core::ForeignInterfaceHost::CaptureManagerImpl::ErrorInfo
                                        error_info{};
        DAS::DasPtr<IDasReadOnlyString> capture_factory_name;
        DasGuid                         factory_iid{};
        try
        {
            capture_factory_name =
                DAS::Core::Utils::GetRuntimeClassNameFrom(p_factory.Get());
            factory_iid = Utils::GetGuidFrom(p_factory.Get());
        }
        catch (const DAS::Core::DasException& ex)
        {
            DAS_CORE_LOG_ERROR(
                "Can not resolve capture factory type name or iid.");
            DAS_CORE_LOG_EXCEPTION(ex);
            result = DAS_FALSE;
            error_info.error_code = ex.GetErrorCode();
            continue;
        }

        DAS::DasPtr<IDasCapture> p_instance{};
        const auto               opt_ref_interface_static_storage =
            plugin_manager.FindInterfaceStaticStorage(factory_iid);
        if (!opt_ref_interface_static_storage)
        {
            DAS_CORE_LOG_ERROR(
                "No matched interface storage! Iid = {}.",
                factory_iid);
            result = DAS_FALSE;
            error_info.error_code = opt_ref_interface_static_storage.error();
            continue;
        }
        DasPtr<IDasReadOnlyString> p_plugin_config{};
        opt_ref_interface_static_storage.value()
            .get()
            .sp_desc->settings_json_->GetValue(p_plugin_config.Put());
        if (const auto error_code = p_factory->CreateInstance(
                p_environment_json_config,
                p_plugin_config.Get(),
                p_instance.Put());
            DAS::IsFailed(error_code))
        {
            result = DAS_S_FALSE;
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
                DAS::Core::Utils::GetRuntimeClassNameFrom(p_instance.Get());
            p_capture_manager->AddInstance(capture_name, p_instance);
        }
        catch (const DAS::Core::DasException& ex)
        {
            DAS_CORE_LOG_ERROR("Get IDasCapture object name failed.");
            DAS_CORE_LOG_EXCEPTION(ex);
            result = DAS_FALSE;
            DAS::DasPtr<IDasReadOnlyString> p_null_string{};
            ::CreateNullDasString(p_null_string.Put());
            p_capture_manager->AddInstance(p_null_string, p_instance);
        }
    }

    return {result, p_capture_manager};
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
