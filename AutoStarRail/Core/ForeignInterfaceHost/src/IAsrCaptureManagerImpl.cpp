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
#include <utility>
#include <vector>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

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

ASR_IMPL IAsrCaptureManagerImpl::EnumCaptureLoadErrorState(
    const size_t         index,
    AsrResult*           p_error_code,
    IAsrReadOnlyString** pp_out_error_explanation)
{
    return impl_.EnumCaptureLoadErrorState(
        index,
        p_error_code,
        pp_out_error_explanation);
}

ASR_IMPL IAsrCaptureManagerImpl::EnumCaptureInterface(
    const size_t  index,
    IAsrCapture** pp_out_interface)
{
    return impl_.EnumCaptureInterface(index, pp_out_interface);
}

ASR_IMPL IAsrCaptureManagerImpl::RunCapturePerformanceTest()
{
    return impl_.RunCapturePerformanceTest();
}

ASR_IMPL IAsrCaptureManagerImpl::EnumCapturePerformanceTestResult(
    const size_t         index,
    AsrResult*           p_out_error_code,
    int32_t*             p_out_time_spent_in_ms,
    IAsrReadOnlyString** pp_out_error_explanation)
{
    return impl_.EnumCapturePerformanceTestResult(
        index,
        p_out_error_code,
        p_out_time_spent_in_ms,
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

AsrRetCapture IAsrSwigCaptureManagerImpl::EnumCaptureInterface(
    const size_t index)
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
    return ASR_E_NO_IMPLEMENTATION;
}

AsrResult CaptureManagerImpl::EnumCapturePerformanceTestResult(
    const size_t         index,
    AsrResult*           p_out_error_code,
    int32_t*             p_out_time_spent_in_ms,
    IAsrReadOnlyString** pp_out_error_explanation)
{
    ErrorInfo* p_error_info{};
    try
    {
        p_error_info = &performance_results_.at(index);
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
    if (p_out_error_code)
    {
        *p_out_error_code = p_error_info->error_code;
    }
    if (p_out_time_spent_in_ms)
    {
        *p_out_time_spent_in_ms = p_error_info->time_spent_in_ms;
    }
    if (pp_out_error_explanation)
    {
        *pp_out_error_explanation = p_error_info->p_error_message.Get();
        (*pp_out_error_explanation)->AddRef();
    }
    return ASR_S_OK;
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

ASR_NS_ANONYMOUS_DETAILS_BEGIN

void OnCreateCaptureInstanceFailed(
    ASR::Core::ForeignInterfaceHost::CaptureManagerImpl::ErrorInfo&
                                           in_error_info,
    const ASR::AsrPtr<IAsrReadOnlyString>& p_capture_factory_name,
    const ASR::AsrPtr<ASR::Core::ForeignInterfaceHost::CaptureManagerImpl>&
        p_capture_manager)
{
    std::string                     error_message;
    ASR::AsrPtr<IAsrTypeInfo>       p_capture_base{};
    ASR::AsrPtr<IAsrReadOnlyString> p_error_message{};
    if (const auto get_error_message_result = ::AsrGetErrorMessage(
            p_capture_base.Get(),
            in_error_info.error_code,
            p_error_message.Put());
        ASR::IsOk(get_error_message_result))
    {
        const char* p_u8_explanation{nullptr};
        p_error_message->GetUtf8(&p_u8_explanation);
        error_message = ASR::fmt::format(
            R"(Error happened when creating capture instance.
FactoryName: {}.
Error code: {}.
Error explanation: "{}".)",
            p_capture_factory_name,
            in_error_info.error_code,
            p_u8_explanation);
        in_error_info.p_error_message = p_error_message;
    }
    else
    {
        error_message = ASR::fmt::format(
            R"(Error happened when creating capture instance.
FactoryName: {}.
Error code: {}.
No error explanation found. Result: {}.)",
            p_capture_factory_name,
            in_error_info.error_code,
            get_error_message_result);
    }
    p_capture_manager->AddInstance(p_capture_factory_name, in_error_info);
    ASR_CORE_LOG_ERROR(error_message);
}

ASR_NS_ANONYMOUS_DETAILS_END

auto CreateAsrCaptureManagerImpl(IAsrReadOnlyString* p_json_config)
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
    const auto& capture_factories =
        ASR::Core::ForeignInterfaceHost::g_plugin_manager
            .GetAllCaptureFactory();
    p_capture_manager->ReserveInstanceContainer(capture_factories.size());
    for (const auto& p_factory : capture_factories)
    {
        ASR::Core::ForeignInterfaceHost::CaptureManagerImpl::ErrorInfo
                                        error_info{};
        ASR::AsrPtr<IAsrReadOnlyString> capture_factory_name;
        try
        {
            capture_factory_name =
                ASR::Core::Utils::GetRuntimeClassNameFrom(p_factory.Get());
        }
        catch (const ASR::Core::AsrException& ex)
        {
            ASR_CORE_LOG_ERROR("Can not resolve capture factory type name.");
            ASR_CORE_LOG_EXCEPTION(ex);

            result = ASR_FALSE;
            continue;
        }

        ASR::AsrPtr<IAsrCapture> p_instance{};

        if (const auto error_code =
                p_factory->CreateInstance(p_json_config, p_instance.Put());
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
