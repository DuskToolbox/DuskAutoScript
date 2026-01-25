#include <das/Core/ForeignInterfaceHost/IDasCaptureManagerImpl.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/i18n/DasResultTranslator.h>
#include <das/DasApi.h>
#include <das/DasConfig.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/Timer.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasCapture.hpp>
#include <das/_autogen/idl/wrapper/IDasTypeInfo.hpp>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto MakeErrorInfo(DasResult error_code, DasTypeInfo p_error_generator)
    -> CaptureManagerImpl::ErrorInfo
{
    CaptureManagerImpl::ErrorInfo result{};
    result.error_code = error_code;
    std::string              error_message{};
    DasReadOnlyStringWrapper das_error_message{};
    const auto               name = p_error_generator.GetRuntimeClassName();
    if (const auto get_error_message_result = ::DasGetErrorMessage(
            p_error_generator.Get(),
            error_code,
            das_error_message.Put());
        DAS::IsOk(get_error_message_result))
    {
        const char* u8_error_message;
        das_error_message.GetTo(u8_error_message);
        error_message = DAS::fmt::format(
            R"(Error happened when creating capture instance.
TypeName: {}.
Error code: {}.
Error explanation: "{}".)",
            name,
            result.error_code,
            u8_error_message);
        result.p_error_message = das_error_message.Get();
    }
    else
    {
        error_message = DAS_FMT_NS::format(
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

auto MakeErrorInfo(
    DasResult           error_code,
    IDasReadOnlyString* p_capture_factory_name) -> CaptureManagerImpl::ErrorInfo
{
    CaptureManagerImpl::ErrorInfo result{};

    result.error_code = error_code;
    const auto error_message = DAS_FMT_NS::format(
        R"(Error happened when creating capture instance.
TypeName: {}.
Error code: {}.)",
        *p_capture_factory_name,
        result.error_code);
    DAS::DasPtr<IDasReadOnlyString> p_error_message{};
    DAS_THROW_IF_FAILED(
        ::CreateIDasReadOnlyStringFromUtf8(
            error_message.c_str(),
            p_error_message.Put()))
    result.p_error_message = p_error_message.Get();
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

DasResult CaptureManagerImpl::EnumLoadErrorState(
    uint64_t             index,
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

DasResult CaptureManagerImpl::EnumInterface(
    const size_t                   index,
    PluginInterface::IDasCapture** pp_out_interface)
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

DasResult CaptureManagerImpl::RunPerformanceTest()
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
        DasPtr<ExportInterface::IDasImage> p_image{};
        ErrorInfo                          capture_error_info{};
        const auto                         p_capture = instance.value();
        DAS::Utils::Timer                  timer{};
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

DasResult CaptureManagerImpl::EnumPerformanceTestResult(
    uint64_t                       index,
    DasResult*                     p_out_error_code,
    int32_t*                       p_out_time_spent_in_ms,
    PluginInterface::IDasCapture** pp_out_capture,
    IDasReadOnlyString**           pp_out_error_explanation)
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

void CaptureManagerImpl::AddInstance(
    DasPtr<IDasReadOnlyString>           p_name,
    DasPtr<PluginInterface::IDasCapture> p_instance)
{
    instances_.emplace_back(
        DasReadOnlyString{std::move(p_name)},
        PluginInterface::DasCapture{std::move(p_instance)});
}

void CaptureManagerImpl::AddInstance(
    DasPtr<IDasReadOnlyString> p_name,
    const ErrorInfo&           error_info)
{
    instances_.emplace_back(
        DasReadOnlyString{std::move(p_name)},
        tl::make_unexpected(error_info));
}

void CaptureManagerImpl::AddInstance(const ErrorInfo& error_info)
{
    instances_.emplace_back(
        DasReadOnlyString{},
        tl::make_unexpected(error_info));
}

void CaptureManagerImpl::ReserveInstanceContainer(
    const std::size_t instance_count)
{
    instances_.reserve(instance_count);
}

// todo: 重写pluginmanager后再实现
// auto CreateDasCaptureManagerImpl(
//     const std::vector<DasPtr<IDasCaptureFactory>>& capture_factories,
//     IDasReadOnlyString*                            p_environment_json_config,
//     PluginManager&                                 plugin_manager)
//     -> std::pair<
//         DasResult,
//         DAS::DasPtr<DAS::Core::ForeignInterfaceHost::DasCaptureManagerImpl>>
// {
//     DAS::DasPtr<IDasReadOnlyString> p_locale_name{};
//     DAS::DasPtr<DAS::Core::ForeignInterfaceHost::DasCaptureManagerImpl>
//               p_capture_manager{};
//     DasResult result{DAS_S_OK};
//
//     try
//     {
//         p_capture_manager = DAS::MakeDasPtr<
//             DAS::Core::ForeignInterfaceHost::DasCaptureManagerImpl>();
//     }
//     catch (std::bad_alloc&)
//     {
//         DAS_CORE_LOG_ERROR("Out of memory!");
//         return {DAS_E_OUT_OF_MEMORY, nullptr};
//     }
//
//     ::DasGetDefaultLocale(p_locale_name.Put());
//     p_capture_manager->ReserveInstanceContainer(capture_factories.size());
//     for (const auto& p_factory : capture_factories)
//     {
//         DAS::Core::ForeignInterfaceHost::DasCaptureManagerImpl::ErrorInfo
//                                         error_info{};
//         DAS::DasPtr<IDasReadOnlyString> capture_factory_name;
//         DasGuid                         factory_iid{};
//         try
//         {
//             capture_factory_name =
//                 DAS::Core::Utils::GetRuntimeClassNameFrom(p_factory.Get());
//             factory_iid = Utils::GetGuidFrom(p_factory.Get());
//         }
//         catch (const DAS::Core::DasException& ex)
//         {
//             DAS_CORE_LOG_ERROR(
//                 "Can not resolve capture factory type name or iid.");
//             DAS_CORE_LOG_EXCEPTION(ex);
//             result = DAS_FALSE;
//             error_info.error_code = ex.GetErrorCode();
//             continue;
//         }
//
//         DAS::DasPtr<IDasCapture> p_instance{};
//         const auto               opt_ref_interface_static_storage =
//             plugin_manager.FindInterfaceStaticStorage(factory_iid);
//         if (!opt_ref_interface_static_storage)
//         {
//             DAS_CORE_LOG_ERROR(
//                 "No matched interface storage! Iid = {}.",
//                 factory_iid);
//             result = DAS_FALSE;
//             error_info.error_code = opt_ref_interface_static_storage.error();
//             continue;
//         }
//         DasPtr<IDasReadOnlyString> p_plugin_config{};
//         opt_ref_interface_static_storage.value()
//             .get()
//             .sp_desc->settings_json_->GetValue(p_plugin_config.Put());
//         if (const auto error_code = p_factory->CreateInstance(
//                 p_environment_json_config,
//                 p_plugin_config.Get(),
//                 p_instance.Put());
//             DAS::IsFailed(error_code))
//         {
//             result = DAS_S_FALSE;
//             error_info.error_code = error_code;
//             Details::OnCreateCaptureInstanceFailed(
//                 p_factory,
//                 error_info,
//                 capture_factory_name,
//                 p_capture_manager);
//             continue;
//         }
//
//         try
//         {
//             const auto capture_name =
//                 DAS::Core::Utils::GetRuntimeClassNameFrom(p_instance.Get());
//             p_capture_manager->AddInstance(capture_name, p_instance);
//         }
//         catch (const DAS::Core::DasException& ex)
//         {
//             DAS_CORE_LOG_ERROR("Get IDasCapture object name failed.");
//             DAS_CORE_LOG_EXCEPTION(ex);
//             result = DAS_FALSE;
//             DAS::DasPtr<IDasReadOnlyString> p_null_string{};
//             ::CreateNullDasString(p_null_string.Put());
//             p_capture_manager->AddInstance(p_null_string, p_instance);
//         }
//     }
//
//     return {result, p_capture_manager};
// }

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
