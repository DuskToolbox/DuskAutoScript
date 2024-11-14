#include "das/Core/Exceptions/PythonException.h"
#include "das/ExportInterface/IDasPluginManager.h"

#include <das/DasString.hpp>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/IDasCaptureManagerImpl.h>
#include <das/Core/ForeignInterfaceHost/IDasPluginManagerImpl.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/TaskScheduler/TaskScheduler.h>
#include <das/Core/Utils/InternalUtils.h>
#include <das/Core/i18n/DasResultTranslator.h>
#include <das/Core/i18n/GlobalLocale.h>
#include <das/IDasBase.h>
#include <das/PluginInterface/IDasErrorLens.h>
#include <das/PluginInterface/IDasTask.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <das/Utils/StreamUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/UnexpectedEnumException.h>
#include <algorithm>
#include <boost/pfr/core.hpp>
#include <fstream>
#include <functional>
#include <magic_enum.hpp>
#include <magic_enum_format.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <utility>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

using CommonBasePtr = std::variant<DasPtr<IDasBase>, DasPtr<IDasSwigBase>>;

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto GetFeaturesFrom(const CommonPluginPtr& p_plugin)
    -> DAS::Utils::Expected<std::vector<DasPluginFeature>>
{
    constexpr size_t MAX_ENUM_COUNT =
        magic_enum::enum_count<DasPluginFeature>() + static_cast<size_t>(30);

    DasPluginFeature feature{};
    auto             result = std::vector<DasPluginFeature>{0};
    result.reserve(magic_enum::enum_count<DasPluginFeature>());

    for (size_t i = 0; i < MAX_ENUM_COUNT; ++i)
    {
        const auto get_feature_result =
            CommonPluginEnumFeature(p_plugin, i, &feature);
        if (IsOk(get_feature_result))
        {
            result.push_back(feature);
            continue;
        }
        if (get_feature_result == DAS_E_OUT_OF_RANGE)
        {
            return result;
        }
        return tl::make_unexpected(get_feature_result);
    }
    DAS_CORE_LOG_WARN(
        "Executing function \"EnumFeature\" in plugin more than the maximum limit of {} times, stopping.",
        MAX_ENUM_COUNT);
    return result;
}

template <class T>
auto GetSupportedInterface(
    T&                                   p_plugin,
    const std::vector<DasPluginFeature>& features)
{
    for (auto feature : features)
    {
        DasPtr<IDasBase> p_interface{};
        auto             get_interface_result =
            p_plugin->GetInterface(feature, &p_interface);
        if (!DAS::IsOk(get_interface_result))
        {
            // TODO: Call internal error lens to interpret the error code.
            DAS_CORE_LOG_ERROR(
                "Get plugin interface for feature {} (value={}) failed: {}",
                magic_enum::enum_name(feature),
                feature,
                get_interface_result);
        }
    }
}

/**
 *
 * @tparam F
 * @param error_code
 * @param p_locale_name
 * @param callback The function return struct { DasResult error_code,
 * DasReadOnlyString value; };
 * @return
 */
template <class F>
auto GetErrorMessageAndAutoFallBack(
    const DasResult     error_code,
    IDasReadOnlyString* p_locale_name,
    F&& callback) -> DAS::Utils::Expected<DAS::DasPtr<IDasReadOnlyString>>
{
    constexpr size_t ERROR_CODE_INDEX = 0;
    constexpr size_t VALUE_INDEX = 1;

    DAS::DasPtr<IDasReadOnlyString> result{};
    auto&&     get_error_message = std::forward<F>(callback);
    const auto result_1 = get_error_message(error_code, p_locale_name);
    const auto result_1_error_code =
        boost::pfr::get<ERROR_CODE_INDEX>(result_1);

    if (DAS::IsOk(result_1_error_code))
    {
        // use default locale and retry.
        if (result_1_error_code == DAS_E_OUT_OF_RANGE)
        {
            const auto p_fallback_locale_name =
                DAS::Core::i18n::GetFallbackLocale();
            const auto result_2 =
                get_error_message(error_code, p_fallback_locale_name.Get());
            if (const auto result_2_error_code =
                    boost::pfr::get<ERROR_CODE_INDEX>(result_2);
                !IsOk(result_2_error_code))
            {
                return tl::make_unexpected(result_2_error_code);
            }
            boost::pfr::get<VALUE_INDEX>(result_2).GetImpl(result.Put());
            return result;
        }
        return tl::make_unexpected(result_1_error_code);
    }

    boost::pfr::get<VALUE_INDEX>(result_1).GetImpl(result.Put());
    return result;
}

auto GetPredefinedErrorMessage(
    const DasResult     error_code,
    IDasReadOnlyString* p_locale_name)
    -> DAS::Utils::Expected<DAS::DasPtr<IDasReadOnlyString>>
{
    // 不是插件自定义错误时
    if (error_code < DAS_E_RESERVED)
    {
        return DAS::Core::ForeignInterfaceHost::Details::
            GetErrorMessageAndAutoFallBack(
                error_code,
                p_locale_name,
                [](auto&& internal_error_code, auto&& internal_p_locale_name)
                {
                    DAS::DasPtr<IDasReadOnlyString> p_result{};
                    DasRetReadOnlyString            result{};
                    result.error_code = DAS::Core::i18n::TranslateError(
                        internal_p_locale_name,
                        internal_error_code,
                        p_result.Put());
                    result.value = DasReadOnlyString{p_result};
                    return result;
                });
    }
    DAS_CORE_LOG_ERROR(
        "The error code {} ({} >= " DAS_STR(
            DAS_E_RESERVED) ") which is not a predefined error code.",
        error_code,
        error_code);
    return tl::make_unexpected(DAS_E_OUT_OF_RANGE);
}

auto CreateInterface(
    const char*            u8_plugin_name,
    const CommonPluginPtr& common_p_plugin,
    size_t                 index) -> std::optional<CommonBasePtr>
{
    constexpr auto& CREATE_FEATURE_INTERFACE_FAILED_MESSAGE =
        "Error happened when calling p_plugin->CreateFeatureInterface."
        "Error code = {}. Plugin Name = {}.";
    return std::visit(
        DAS::Utils::overload_set{
            [index, u8_plugin_name](
                DasPtr<IDasPlugin> p_plugin) -> std::optional<CommonBasePtr>
            {
                DasPtr<IDasBase> result{};
                if (const auto cfi_result = p_plugin->CreateFeatureInterface(
                        index,
                        result.PutVoid());
                    IsFailed(cfi_result))
                {
                    DAS_CORE_LOG_ERROR(
                        CREATE_FEATURE_INTERFACE_FAILED_MESSAGE,
                        cfi_result,
                        u8_plugin_name);
                    return {};
                }
                return result;
            },
            [index, u8_plugin_name](
                DasPtr<IDasSwigPlugin> p_plugin) -> std::optional<CommonBasePtr>
            {
                DasRetSwigBase cfi_result{};
                cfi_result = p_plugin->CreateFeatureInterface(index);
                if (IsFailed(cfi_result.error_code))
                {
                    DAS_CORE_LOG_ERROR(
                        CREATE_FEATURE_INTERFACE_FAILED_MESSAGE,
                        cfi_result.error_code,
                        u8_plugin_name);
                    return {};
                }
                DasPtr<IDasSwigBase> result{reinterpret_cast<IDasSwigBase*>(
                    cfi_result.GetVoidNoAddRef())};
                return result;
            }},
        common_p_plugin);
}

auto QueryTypeInfoFrom(
    const char*          p_plugin_name,
    const CommonBasePtr& common_p_base) -> std::optional<CommonTypeInfoPtr>
{
    constexpr auto& QUERY_TYPE_INFO_FAILED_MESSAGE =
        "Failed when querying IDasTypeInfo or IDasSwigTypeInfo. ErrorCode = {}. Pointer = {}. Plugin name = {}.";
    return std::visit(
        DAS::Utils::overload_set{
            [p_plugin_name](const DasPtr<IDasBase>& p_base)
                -> std::optional<CommonTypeInfoPtr>
            {
                DasPtr<IDasTypeInfo> result;
                if (const auto qi_result = p_base.As(result);
                    IsFailed(qi_result))
                {
                    DAS_CORE_LOG_ERROR(
                        QUERY_TYPE_INFO_FAILED_MESSAGE,
                        qi_result,
                        static_cast<void*>(p_base.Get()),
                        p_plugin_name);
                    return {};
                }
                return result;
            },
            [p_plugin_name](const DasPtr<IDasSwigBase>& p_base)
                -> std::optional<CommonTypeInfoPtr>
            {
                const auto qi_result =
                    p_base->QueryInterface(DasIidOf<IDasSwigTypeInfo>());
                if (IsFailed(qi_result.error_code))
                {
                    DAS_CORE_LOG_ERROR(
                        QUERY_TYPE_INFO_FAILED_MESSAGE,
                        qi_result.error_code,
                        static_cast<void*>(p_base.Get()),
                        p_plugin_name);
                    return {};
                }
                DasPtr<IDasSwigTypeInfo> result{static_cast<IDasSwigTypeInfo*>(
                    qi_result.GetVoidNoAddRef())};
                return result;
            }},
        common_p_base);
}

constexpr auto& GET_GUID_FAILED_MESSAGE =
    "Get guid from interface failed. Error code = {}";

auto GetInterfaceStaticStorage(
    const PluginManager::InterfaceStaticStorageMap& guid_storage_map,
    const DasGuid&                                  guid)
    -> DAS::Utils::Expected<
        std::reference_wrapper<const PluginManager::InterfaceStaticStorage>>
{
    const auto it_storage = guid_storage_map.find(guid);
    if (it_storage == guid_storage_map.end())
    {
        DAS_CORE_LOG_ERROR(
            "No vaild interface static storage foud. Guid = {}.",
            guid);
    }

    return std::cref(it_storage->second);
}

DAS_NS_ANONYMOUS_DETAILS_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

DasResult DasGetErrorMessage(
    IDasTypeInfo*        p_error_generator,
    DasResult            error_code,
    IDasReadOnlyString** pp_out_error_message)
{
    DasGuid guid;
    if (const auto get_guid_result = p_error_generator->GetGuid(&guid);
        DAS::IsFailed(get_guid_result))
    {
        return get_guid_result;
    }

    return Das::Core::ForeignInterfaceHost::g_plugin_manager.GetErrorMessage(
        guid,
        error_code,
        pp_out_error_message);
}

DasResult DasGetPredefinedErrorMessage(
    DasResult            error_code,
    IDasReadOnlyString** pp_out_error_message)
{
    DAS::DasPtr<IDasReadOnlyString> p_default_locale_name{};
    std::ignore = ::DasGetDefaultLocale(p_default_locale_name.Put());
    const auto result =
        DAS::Core::ForeignInterfaceHost::Details::GetPredefinedErrorMessage(
            error_code,
            p_default_locale_name.Get());
    if (result)
    {
        auto* const p_result = result.value().Get();
        p_result->AddRef();
        *pp_out_error_message = p_result;
        return DAS_S_OK;
    }
    return result.error();
}

DasRetReadOnlyString DasGetErrorMessage(
    IDasSwigTypeInfo* p_error_generator,
    DasResult         error_code)
{
    DasRetReadOnlyString            result{};
    DAS::DasPtr<IDasReadOnlyString> p_result;

    const auto ret_guid = p_error_generator->GetGuid();
    if (DAS::IsFailed(ret_guid.error_code))
    {
        result.error_code = ret_guid.error_code;
    }
    result.error_code =
        DAS::Core::ForeignInterfaceHost::g_plugin_manager.GetErrorMessage(
            ret_guid.value,
            error_code,
            p_result.Put());

    return result;
}

DasRetReadOnlyString DasGetPredefinedErrorMessage(DasResult error_code)
{
    DasRetReadOnlyString            result{};
    DAS::DasPtr<IDasReadOnlyString> p_result{};
    result.error_code =
        DasGetPredefinedErrorMessage(error_code, p_result.Put());
    if (DAS::IsOk(result.error_code))
    {
        result.value = p_result;
    }
    return result;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_DEFINE_VARIABLE(g_plugin_manager){};

DAS_NS_ANONYMOUS_DETAILS_BEGIN

/**
 *
 * @tparam T The type of interface
 * @tparam SwigT The type of swig interface
 * @tparam N auto padding
 * @param error_message Error message. Param will pass error_code, p_base and
 * plugin_name
 * @param u8_plugin_name The plugin_name
 * @param common_p_base The p_base
 * @return Expected
 */
template <class T, class SwigT, size_t N>
auto QueryInterfaceFrom(
    const char (&error_message)[N],
    const char*          u8_plugin_name,
    const CommonBasePtr& common_p_base) -> DAS::Utils::Expected<DasPtr<T>>
{
    using RetType = DAS::Utils::Expected<DasPtr<T>>;

    return std::visit(
        DAS::Utils::overload_set{
            [&error_message,
             u8_plugin_name](const DasPtr<IDasBase>& p_base) -> RetType
            {
                DasPtr<T> result;
                if (const auto qi_result = p_base.As(result);
                    DAS::IsFailed(qi_result))
                {
                    DAS_CORE_LOG_ERROR(
                        error_message,
                        qi_result,
                        static_cast<void*>(p_base.Get()),
                        u8_plugin_name);
                    return tl::make_unexpected(qi_result);
                }

                return result;
            },
            [&error_message,
             u8_plugin_name](const DasPtr<IDasSwigBase>& p_base) -> RetType
            {
                auto qi_result = p_base->QueryInterface(DasIidOf<SwigT>());
                if (DAS::IsFailed(qi_result.error_code))
                {
                    DAS_CORE_LOG_ERROR(
                        error_message,
                        qi_result.error_code,
                        static_cast<void*>(p_base.Get()),
                        u8_plugin_name);
                    return tl::make_unexpected(qi_result.error_code);
                }

                auto result = MakeDasPtr<SwigToCpp<SwigT>>(
                    static_cast<IDasSwigErrorLens*>(
                        qi_result.GetVoidNoAddRef()));

                qi_result.value.Get()->Release();

                return result;
            }},
        common_p_base);
}

auto QueryErrorLensFrom(
    const char*          u8_plugin_name,
    const CommonBasePtr& common_p_base)
    -> DAS::Utils::Expected<DasPtr<IDasErrorLens>>
{
    using RetType = DAS::Utils::Expected<DasPtr<IDasErrorLens>>;

    constexpr const auto& QUERY_ERROR_LENS_FAILED_MESSAGE =
        "Failed when calling QueryInterface. ErrorCode = {}. Pointer = {}. Plugin name = {}.";

    return std::visit(
        DAS::Utils::overload_set{
            [u8_plugin_name](const DasPtr<IDasBase>& p_base) -> RetType
            {
                DasPtr<IDasErrorLens> result;
                if (const auto qi_result = p_base.As(result);
                    DAS::IsFailed(qi_result))
                {
                    DAS_CORE_LOG_ERROR(
                        QUERY_ERROR_LENS_FAILED_MESSAGE,
                        qi_result,
                        static_cast<void*>(p_base.Get()),
                        u8_plugin_name);
                    return tl::make_unexpected(qi_result);
                }

                return result;
            },
            [u8_plugin_name](const DasPtr<IDasSwigBase>& p_base) -> RetType
            {
                auto qi_result =
                    p_base->QueryInterface(DasIidOf<IDasSwigErrorLens>());
                if (DAS::IsFailed(qi_result.error_code))
                {
                    DAS_CORE_LOG_ERROR(
                        QUERY_ERROR_LENS_FAILED_MESSAGE,
                        qi_result.error_code,
                        static_cast<void*>(p_base.Get()),
                        u8_plugin_name);
                    return tl::make_unexpected(qi_result.error_code);
                }

                DasPtr<IDasErrorLens> result{new SwigToCpp<IDasSwigErrorLens>{
                    static_cast<IDasSwigErrorLens*>(
                        qi_result.GetVoidNoAddRef())}};

                return result;
            }},
        common_p_base);
}

struct GetInterfaceFromPluginParam
{
    const char*          u8_plugin_name;
    const CommonBasePtr& p_base;
};

template <class T>
auto RegisterErrorLensFromPlugin(
    T&                          error_lens_manager,
    GetInterfaceFromPluginParam param) -> DasResult
{
    const auto& [u8_plugin_name, common_p_base] = param;

    DasPtr<IDasReadOnlyGuidVector> p_guid_vector{};
    const auto                     expected_p_error_lens =
        QueryErrorLensFrom(u8_plugin_name, common_p_base);
    if (!expected_p_error_lens)
    {
        return expected_p_error_lens.error();
    }

    const auto& p_error_lens = expected_p_error_lens.value();

    if (const auto get_iids_result =
            p_error_lens->GetSupportedIids(p_guid_vector.Put());
        DAS::IsFailed(get_iids_result))
    {
        DAS_CORE_LOG_ERROR(
            "Try to get supported iids failed. Error code = {}. Plugin name = {}.",
            get_iids_result,
            u8_plugin_name);
        return get_iids_result;
    }
    error_lens_manager.Register(p_guid_vector.Get(), p_error_lens.Get());
    return DAS_S_OK;
}

auto RegisterTaskFromPlugin(
    TaskManager&                task_manager,
    std::shared_ptr<PluginDesc> sp_plugin_desc,
    GetInterfaceFromPluginParam param) -> DasResult
{
    const auto& [u8_plugin_name, common_p_base] = param;

    using CommonTaskPointer =
        std::variant<DasPtr<IDasTask>, DasPtr<IDasSwigTask>>;
    using ExpectedCommonTaskPointer = DAS::Utils::Expected<CommonTaskPointer>;

    ExpectedCommonTaskPointer expected_common_p_task = std::visit(
        DAS::Utils::overload_set{
            [](const DasPtr<IDasBase>& p_base) -> ExpectedCommonTaskPointer
            {
                DasPtr<IDasTask> p_task{};
                if (const auto qi_result = p_base.As(p_task);
                    IsFailed(qi_result))
                {
                    return tl::make_unexpected(qi_result);
                }
                return p_task;
            },
            [](const DasPtr<IDasSwigBase>& p_base) -> ExpectedCommonTaskPointer
            {
                try
                {
                    const auto qi_result =
                        p_base->QueryInterface(DasIidOf<IDasSwigTask>());
                    if (IsFailed(qi_result.error_code))
                    {
                        return tl::make_unexpected(qi_result.error_code);
                    }
                    return DasPtr{static_cast<IDasSwigTask*>(
                        qi_result.GetVoidNoAddRef())};
                }
                catch (...)
                {
                    return tl::make_unexpected(DAS_E_INTERNAL_FATAL_ERROR);
                }
            }},
        common_p_base);
    if (!expected_common_p_task)
    {
        return expected_common_p_task.error();
    }

    return std::visit(
        DAS::Utils::overload_set{
            [u8_plugin_name, &task_manager, sp_plugin_desc](
                const DasPtr<IDasTask>& p_task)
            {
                try
                {
                    const auto guid = Utils::GetGuidFrom(p_task.Get());
                    return task_manager.Register(
                        sp_plugin_desc,
                        p_task.Get(),
                        guid);
                }
                catch (const DasException& ex)
                {
                    DAS_CORE_LOG_ERROR(
                        "Get guid in IDasTask object failed."
                        "Plugin name = {}. Error Code = {}. Error message = {}.",
                        u8_plugin_name,
                        ex.GetErrorCode(),
                        ex.what());
                    return ex.GetErrorCode();
                }
            },
            [u8_plugin_name, &task_manager, sp_plugin_desc](
                const DasPtr<IDasSwigTask>& p_task)
            {
                try
                {
                    const auto guid = Utils::GetGuidFrom(p_task.Get());
                    return task_manager.Register(
                        sp_plugin_desc,
                        p_task.Get(),
                        guid);
                }
                catch (const DasException& ex)
                {
                    DAS_CORE_LOG_ERROR(
                        "Get guid in IDasSwigTask object failed."
                        "Plugin name = {}. Error Code = {}. Error message = {}.",
                        u8_plugin_name,
                        ex.GetErrorCode(),
                        ex.what());
                    return ex.GetErrorCode();
                }
            }},
        expected_common_p_task.value());
}

template <class T>
auto RegisterCaptureFactoryFromPlugin(
    T&                          capture_factory_vector,
    GetInterfaceFromPluginParam param) -> DasResult
{
    const auto& [u8_plugin_name, common_p_base] = param;

    const auto pp_base = std::get_if<DasPtr<IDasBase>>(&common_p_base);
    if (pp_base == nullptr) [[unlikely]]
    {
        DAS_CORE_LOG_ERROR(
            "Variable common_p_base does NOT contain DasPtr<IDasBase> object. Plugin name = {}.",
            u8_plugin_name);
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
    const auto&                p_base = *pp_base;
    DasPtr<IDasCaptureFactory> p_result{};
    if (const auto qi_result = p_base.As(p_result); IsFailed(qi_result))
    {
        DAS_CORE_LOG_ERROR(
            "Can not convert interface to IDasCaptureFactory. Plugin name = {}. Error code = {}.",
            u8_plugin_name,
            qi_result);
        return qi_result;
    }
    capture_factory_vector.emplace_back(std::move(p_result));
    return DAS_S_OK;
}

auto RegisterInputFactoryFromPlugin(
    InputFactoryManager&        input_factory_manager,
    GetInterfaceFromPluginParam param) -> DasResult
{
    const auto& [u8_plugin_name, common_p_base] = param;

    using CommonInputFactoryPointer =
        std::variant<DasPtr<IDasInputFactory>, DasPtr<IDasSwigInputFactory>>;
    using ExpectedCommonInputFactoryPointer =
        DAS::Utils::Expected<CommonInputFactoryPointer>;

    ExpectedCommonInputFactoryPointer expected_common_p_input_factory =
        std::visit(
            DAS::Utils::overload_set{
                [](const DasPtr<IDasBase>& p_base)
                    -> ExpectedCommonInputFactoryPointer
                {
                    DasPtr<IDasInputFactory> p_task{};
                    if (const auto qi_result = p_base.As(p_task);
                        IsFailed(qi_result))
                    {
                        return tl::make_unexpected(qi_result);
                    }
                    return p_task;
                },
                [](const DasPtr<IDasSwigBase>& p_base)
                    -> ExpectedCommonInputFactoryPointer
                {
                    const auto qi_result = p_base->QueryInterface(
                        DasIidOf<IDasSwigInputFactory>());
                    if (IsFailed(qi_result.error_code))
                    {
                        return tl::make_unexpected(qi_result.error_code);
                    }
                    return DasPtr{static_cast<IDasSwigInputFactory*>(
                        qi_result.GetVoidNoAddRef())};
                }},
            common_p_base);

    if (!expected_common_p_input_factory)
    {
        return expected_common_p_input_factory.error();
    }

    return std::visit(
        Utils::overload_set{
            [&input_factory_manager](const DasPtr<IDasInputFactory>& p_factory)
            { return input_factory_manager.Register(p_factory.Get()); },
            [&input_factory_manager](
                const DasPtr<IDasSwigInputFactory>& p_factory)
            { return input_factory_manager.Register(p_factory.Get()); }},
        expected_common_p_input_factory.value());
}

auto RegisterComponentFactoryFromPlugin(
    ComponentFactoryManager&    component_factory_manager,
    GetInterfaceFromPluginParam param) -> DasResult
{
    const auto& [u8_plugin_name, common_p_base] = param;

    using CommonComponentFactoryPointer = std::
        variant<DasPtr<IDasComponentFactory>, DasPtr<IDasSwigComponentFactory>>;
    using ExpectedCommonComponentFactoryPointer =
        DAS::Utils::Expected<CommonComponentFactoryPointer>;

    ExpectedCommonComponentFactoryPointer expected_common_p_component_factory =
        std::visit(
            DAS::Utils::overload_set{
                [](const DasPtr<IDasBase>& p_base)
                    -> ExpectedCommonComponentFactoryPointer
                {
                    DasPtr<IDasComponentFactory> p_component_factory{};
                    if (const auto qi_result = p_base.As(p_component_factory);
                        IsFailed(qi_result))
                    {
                        return tl::make_unexpected(qi_result);
                    }
                    return p_component_factory;
                },
                [](const DasPtr<IDasSwigBase>& p_base)
                    -> ExpectedCommonComponentFactoryPointer
                {
                    const auto qi_result = p_base->QueryInterface(
                        DasIidOf<IDasSwigComponentFactory>());
                    if (IsFailed(qi_result.error_code))
                    {
                        return tl::make_unexpected(qi_result.error_code);
                    }
                    return DasPtr{static_cast<IDasSwigComponentFactory*>(
                        qi_result.GetVoidNoAddRef())};
                }},
            common_p_base);

    if (!expected_common_p_component_factory)
    {
        return expected_common_p_component_factory.error();
    }

    return std::visit(
        Utils::overload_set{[&component_factory_manager](const auto& value) {
            return component_factory_manager.Register(value.Get());
        }},
        expected_common_p_component_factory.value());
}

const std::string UPPER_CURRENT_PLATFORM = []
{
    std::string result{DAS_PLATFORM};
    std::transform(
        DAS_FULL_RANGE_OF(result),
        result.begin(),
        [](unsigned char c) { return toupper(c); });
    return result;
}();

DAS_NS_ANONYMOUS_DETAILS_END

IDasPluginManagerForUiImpl::IDasPluginManagerForUiImpl(PluginManager& impl)
    : impl_{impl}
{
}

int64_t IDasPluginManagerForUiImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasPluginManagerForUiImpl::Release() { return impl_.Release(); }

DasResult IDasPluginManagerForUiImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    const auto qi_ui_result =
        Utils::QueryInterface<IDasPluginManagerForUi>(this, iid, pp_object);
    if (IsOk(qi_ui_result))
    {
        return qi_ui_result;
    }

    const auto qi_result =
        Utils::QueryInterface<IDasPluginManager, IDasPluginManagerImpl>(
            impl_,
            iid,
            pp_object);
    return qi_result;
}

DasResult IDasPluginManagerForUiImpl::GetAllPluginInfo(
    IDasPluginInfoVector** pp_out_plugin_info_vector)
{
    return impl_.GetAllPluginInfo(pp_out_plugin_info_vector);
}

DasResult IDasPluginManagerForUiImpl::FindInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    return impl_.FindInterface(iid, pp_object);
}

DasResult IDasPluginManagerForUiImpl::GetPluginSettingsJson(
    const DasGuid&       plugin_guid,
    IDasReadOnlyString** pp_out_json)
{
    return impl_.GetPluginSettingsJson(plugin_guid, pp_out_json);
}

DasResult IDasPluginManagerForUiImpl::SetPluginSettingsJson(
    const DasGuid&      plugin_guid,
    IDasReadOnlyString* p_json)
{
    return impl_.SetPluginSettingsJson(plugin_guid, p_json);
}

DasResult IDasPluginManagerForUiImpl::ResetPluginSettings(
    const DasGuid& plugin_guid)
{
    return impl_.ResetPluginSettings(plugin_guid);
}

IDasPluginManagerImpl::IDasPluginManagerImpl(PluginManager& impl) : impl_{impl}
{
}

int64_t IDasPluginManagerImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasPluginManagerImpl::Release() { return impl_.Release(); }

DasResult IDasPluginManagerImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    return Utils::QueryInterface<IDasPluginManager>(this, iid, pp_object);
}

DasResult IDasPluginManagerImpl::CreateComponent(
    const DasGuid&  iid,
    IDasComponent** pp_out_component)
{
    return impl_.CreateComponent(iid, pp_out_component);
}

DasResult IDasPluginManagerImpl::CreateCaptureManager(
    IDasReadOnlyString*  p_environment_config,
    IDasCaptureManager** pp_out_capture_manager)
{
    return g_plugin_manager.CreateCaptureManager(
        p_environment_config,
        pp_out_capture_manager);
}

IDasSwigPluginManagerImpl::IDasSwigPluginManagerImpl(PluginManager& impl)
    : impl_{impl}
{
}
int64_t IDasSwigPluginManagerImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasSwigPluginManagerImpl::Release() { return impl_.Release(); }

DasRetSwigBase IDasSwigPluginManagerImpl::QueryInterface(const DasGuid& iid)
{
    return Utils::QueryInterface<IDasSwigPluginManager>(this, iid);
}

DasRetComponent IDasSwigPluginManagerImpl::CreateComponent(const DasGuid& iid)
{
    return impl_.CreateComponent(iid);
}

DasRetCaptureManager IDasSwigPluginManagerImpl::CreateCaptureManager(
    DasReadOnlyString environment_config)
{
    return impl_.CreateCaptureManager(environment_config);
}

DasResult PluginManager::AddInterface(
    const Plugin& plugin,
    const char*   u8_plugin_name)
{
    DasResult   result{DAS_S_OK};
    const auto& common_p_plugin = plugin.p_plugin_;
    const auto& opt_resource_path = plugin.sp_desc_->opt_resource_path;

    const auto expected_features = Details::GetFeaturesFrom(common_p_plugin);
    if (!expected_features)
    {
        return expected_features.error();
    }
    size_t index = 0;
    for (const auto& features = expected_features.value();
         const auto  feature : features)
    {
        const auto opt_common_p_base =
            Details::CreateInterface(u8_plugin_name, common_p_plugin, index);
        ++index;

        if (!opt_common_p_base)
        {
            // NOTE: Error message will be printed by CreateInterface.
            continue;
        }

        if (opt_resource_path)
        {
            if (const auto opt_common_p_type_info = Details::QueryTypeInfoFrom(
                    u8_plugin_name,
                    opt_common_p_base.value()))
            {
                const auto&   relative_path = opt_resource_path.value();
                std::u8string u8_relative_path{
                    DAS_FULL_RANGE_OF(relative_path)};
                InterfaceStaticStorage storage{
                    {std::filesystem::current_path() / u8_relative_path},
                    plugin.sp_desc_};
                std::visit(
                    DAS::Utils::overload_set{
                        [this,
                         &storage](const DasPtr<IDasTypeInfo>& p_type_info) {
                            RegisterInterfaceStaticStorage(
                                p_type_info.Get(),
                                storage);
                        },
                        [this, &storage](
                            const DasPtr<IDasSwigTypeInfo>& p_type_info) {
                            RegisterInterfaceStaticStorage(
                                p_type_info.Get(),
                                storage);
                        }},
                    opt_common_p_type_info.value());
            }
        }

        switch (feature)
        {
        case DAS_PLUGIN_FEATURE_ERROR_LENS:
        {
            if (const auto relfp_result = Details::RegisterErrorLensFromPlugin(
                    error_lens_manager_,
                    {u8_plugin_name, opt_common_p_base.value()});
                IsFailed(relfp_result))
            {
                DAS_CORE_LOG_ERROR(
                    "Can not get error lens interface from plugin {}. "
                    "Error code = {}.",
                    u8_plugin_name,
                    relfp_result);
                result = DAS_S_FALSE;
            }
            break;
        }
        case DAS_PLUGIN_FEATURE_TASK:
        {
            if (const auto rtfp_result = Details::RegisterTaskFromPlugin(
                    task_manager_,
                    plugin.sp_desc_,
                    {u8_plugin_name, opt_common_p_base.value()});
                IsFailed(rtfp_result))
            {
                DAS_CORE_LOG_ERROR(
                    "Can not get task interface from plugin {}. "
                    "Error code = {}.",
                    u8_plugin_name,
                    rtfp_result);
                result = DAS_S_FALSE;
            }
            break;
        }
        case DAS_PLUGIN_FEATURE_CAPTURE_FACTORY:
        {
            if (const auto rcffp_result =
                    Details::RegisterCaptureFactoryFromPlugin(
                        capture_factory_vector_,
                        {u8_plugin_name, opt_common_p_base.value()});
                IsFailed(rcffp_result))
            {
                DAS_CORE_LOG_ERROR(
                    "Can not get capture factory interface from plugin {}. "
                    "Error code = {}.",
                    u8_plugin_name,
                    rcffp_result);
                result = DAS_S_FALSE;
            }
            break;
        }
        case DAS_PLUGIN_FEATURE_INPUT_FACTORY:
        {
            if (const auto riffp_result =
                    Details::RegisterInputFactoryFromPlugin(
                        input_factory_manager_,
                        {u8_plugin_name, opt_common_p_base.value()});
                IsFailed(riffp_result))
            {
                DAS_CORE_LOG_ERROR(
                    "Can not get input factory interface from plugin {}. "
                    "Error code = {}.",
                    u8_plugin_name,
                    riffp_result);
                result = DAS_S_FALSE;
            }
            break;
        }
        case DAS_PLUGIN_FEATURE_COMPONENT_FACTORY:
            if (const auto rcffp_result =
                    Details::RegisterComponentFactoryFromPlugin(
                        component_factory_manager_,
                        {u8_plugin_name, opt_common_p_base.value()});
                IsFailed(rcffp_result))
            {
                DAS_CORE_LOG_ERROR(
                    "Can not get component factory interface from plugin {}. "
                    "Error code = {}.",
                    u8_plugin_name,
                    rcffp_result);
                result = DAS_S_FALSE;
            }
            break;
        default:
            throw DAS::Utils::UnexpectedEnumException::FromEnum(feature);
        }
    }
    return result;
}

void PluginManager::RegisterInterfaceStaticStorage(
    IDasTypeInfo*                 p_interface,
    const InterfaceStaticStorage& storage)
{
    DasGuid guid;
    if (const auto get_guid_result = p_interface->GetGuid(&guid);
        IsFailed(get_guid_result))
    {
        DAS_CORE_LOG_ERROR(Details::GET_GUID_FAILED_MESSAGE, get_guid_result);
    }

    guid_storage_map_[guid] = storage;
}

void PluginManager::RegisterInterfaceStaticStorage(
    IDasSwigTypeInfo*             p_swig_interface,
    const InterfaceStaticStorage& storage)
{
    const auto ret_guid = p_swig_interface->GetGuid();
    if (IsFailed(ret_guid.error_code))
    {
        DAS_CORE_LOG_ERROR(
            Details::GET_GUID_FAILED_MESSAGE,
            ret_guid.error_code);
    }

    guid_storage_map_[ret_guid.value] = storage;
}

std::unique_ptr<PluginDesc> PluginManager::GetPluginDesc(
    const std::filesystem::path& metadata_path,
    bool                         is_directory)
{
    std::unique_ptr<PluginDesc> result{};
    std::ifstream               plugin_config_stream{};

    DAS::Utils::EnableStreamException(
        plugin_config_stream,
        std::ios::badbit | std::ios::failbit,
        [&metadata_path](auto& stream) { stream.open(metadata_path.c_str()); });

    const auto config = nlohmann::json::parse(plugin_config_stream);
    result = std::make_unique<PluginDesc>(config.get<PluginDesc>());

    if (!is_directory)
    {
        result->opt_resource_path = {};
    }

    return result;
}

DasResult PluginManager::GetInterface(const Plugin& plugin)
{
    (void)plugin;
    return DAS_E_NO_IMPLEMENTATION;
}

DAS_NS_ANONYMOUS_DETAILS_BEGIN

/**
 * @brief 插件创建失败时，产生这个类，负责生成报错的信息和错误状态的插件
 */
struct FailedPluginProxy : public DAS::Utils::NonCopyableAndNonMovable
{
    DasPtr<IDasReadOnlyString> error_message;
    DasPtr<IDasReadOnlyString> name;
    DasResult                  error_code;

    FailedPluginProxy(
        const std::filesystem::path& metadata_path,
        DasResult                    error_code)
        : error_message{[](DasResult error_code)
                        {
                            DasPtr<IDasReadOnlyString> result{};
                            DasGetPredefinedErrorMessage(
                                error_code,
                                result.Put());
                            return result;
                        }(error_code)},
          name{MakeDasPtr<IDasReadOnlyString, ::DasStringCppImpl>(
              metadata_path)},
          error_code{error_code}
    {
    }

    FailedPluginProxy(IDasReadOnlyString* plugin_name, DasResult error_code)
        : error_message{[](DasResult error_code)
                        {
                            DasPtr<IDasReadOnlyString> result{};
                            DasGetPredefinedErrorMessage(
                                error_code,
                                result.Put());
                            return result;
                        }(error_code)},
          name{plugin_name}, error_code{error_code}
    {
    }

    void AddPluginTo(PluginManager::NamePluginMap& map)
    {
        map.emplace(name, Plugin{error_code, error_message.Get()});
    }
};

DAS_NS_ANONYMOUS_DETAILS_END

int64_t PluginManager::AddRef()
{
    ++ref_counter_;
    return ref_counter_;
}

int64_t PluginManager::Release()
{
    --ref_counter_;
    return ref_counter_;
}

DasResult PluginManager::Refresh(IDasReadOnlyGuidVector* p_ignored_guid_vector)
{
    if (IsUnexpectedThread())
    {
        return DAS_E_UNEXPECTED_THREAD_DETECTED;
    }
    std::lock_guard _{mutex_};
    DasResult       result{DAS_S_OK};

    const auto ignored_guid_set = [p_ignored_guid_vector]
    {
        std::unordered_set<DasGuid> lambda_result{};
        size_t                      i{0};
        DasGuid                     guid;
        while (true)
        {
            const auto error_code = p_ignored_guid_vector->At(i, &guid);
            if (IsOk(error_code))
            {
                ++i;
                lambda_result.emplace(guid);
                continue;
            }

            if (error_code != DAS_E_OUT_OF_RANGE)
            {
                DAS_CORE_LOG_ERROR(
                    "Unexpected error happened when reading ignored guid."
                    "Error code = {}.",
                    error_code);
            }

            break;
        }
        return lambda_result;
    }();

    NamePluginMap map{};

    for (const auto current_path =
             std::filesystem::canonical(std::filesystem::path{"./plugins"});
         const auto& it : std::filesystem::directory_iterator{current_path})
    {
        std::filesystem::path it_path;
        if (it.is_directory())
        {
            const auto plugin_metadata_name = it.path().filename();
            it_path =
                it
                / std::filesystem::path{
                    plugin_metadata_name.u8string() + std::u8string{u8".json"}};
        }
        else
        {
            it_path = it.path();
        }
        const auto extension = it_path.extension();
        if (DAS_UTILS_STRINGUTILS_COMPARE_STRING(extension, ".json"))
        {
            DasResult plugin_create_result{DAS_E_UNDEFINED_RETURN_VALUE};
            std::unique_ptr<PluginDesc> up_plugin_desc{};

            try
            {
                auto tmp_up_plugin_desc =
                    GetPluginDesc(it_path, it.is_directory());
                up_plugin_desc = std::move(tmp_up_plugin_desc);

                if (ignored_guid_set.contains(up_plugin_desc->guid))
                {
                    continue;
                }

                if (const auto upper_supported_system =
                        Utils::ToUpper(up_plugin_desc->supported_system);
                    upper_supported_system.find_first_of(
                        Details::UPPER_CURRENT_PLATFORM)
                    == decltype(up_plugin_desc->supported_system)::npos)
                {
                    plugin_create_result = DAS_E_UNSUPPORTED_SYSTEM;

                    auto failed_plugin = Details::FailedPluginProxy{
                        it_path,
                        plugin_create_result};

                    DAS_CORE_LOG_ERROR(
                        "Error when checking system requirement. Error code = " DAS_STR(
                            DAS_E_UNSUPPORTED_SYSTEM) ".");
                    // 此处，plugin_name即为metadata路径，见
                    // Details::AddFailedPluginAndReturnTmpPluginName
                    DAS_CORE_LOG_ERROR(
                        "NOTE: plugin meta data file path:\"{}\"",
                        failed_plugin.name);

                    failed_plugin.AddPluginTo(map);
                    result = DAS_S_FALSE;
                    continue;
                }
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                DAS_CORE_LOG_INFO(
                    "Error happened when parsing plugin metadata file. Error code = " DAS_STR(
                        DAS_CORE_LOG_INFO) ".");
                plugin_create_result = DAS_E_INVALID_JSON;
            }
            catch (const std::ios_base::failure& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                DAS_CORE_LOG_INFO(
                    "Error happened when reading plugin metadata file. Error code = " DAS_STR(
                        DAS_E_INVALID_FILE) ".");
                plugin_create_result = DAS_E_INVALID_FILE;
            }

            if (plugin_create_result != DAS_E_UNDEFINED_RETURN_VALUE)
            {
                auto failed_plugin =
                    Details::FailedPluginProxy{it_path, plugin_create_result};
                DAS_CORE_LOG_INFO(
                    "NOTE: plugin meta data file path:\"{}\"",
                    failed_plugin.name);

                failed_plugin.AddPluginTo(map);
                result = DAS_S_FALSE;
                continue;
            }

            DasPtr<IDasReadOnlyString> plugin_name{};

            if (auto expected_plugin_name =
                    DAS::Core::Utils::MakeDasReadOnlyStringFromUtf8(
                        up_plugin_desc->name);
                !expected_plugin_name)
            {
                const auto error_code = expected_plugin_name.error();
                DAS_CORE_LOG_ERROR(
                    "Can not convert std::string to IDasReadOnlyString when getting plugin name."
                    "\nError code = {}",
                    error_code);
                auto failed_plugin =
                    Details::FailedPluginProxy{it_path, error_code};
                failed_plugin.AddPluginTo(map);
                result = DAS_S_FALSE;
                continue;
            }
            else
            {
                plugin_name = expected_plugin_name.value();
            }

            std::filesystem::path plugin_path =
                it_path.parent_path()
                / std::filesystem::path{
                    std::u8string{DAS_FULL_RANGE_OF(up_plugin_desc->name)}
                    + std::u8string{u8"."}
                    + std::u8string{DAS_FULL_RANGE_OF(
                        up_plugin_desc->plugin_filename_extension)}};
            if (!std::filesystem::exists(plugin_path))
            {
                plugin_create_result = DAS_E_FILE_NOT_FOUND;

                DAS_CORE_LOG_ERROR(
                    "Error when checking plugin file. Error code = " DAS_STR(
                        DAS_E_FILE_NOT_FOUND) ".");

                Details::FailedPluginProxy failed_plugin{
                    plugin_name.Get(),
                    plugin_create_result};

                const auto u8_plugin_path = plugin_path.u8string();
                DAS_CORE_LOG_INFO(
                    "NOTE: Plugin metadata file path:\"{}\". "
                    "Expected plugin file path \"{}\"",
                    MakeDasPtr<IDasReadOnlyString, DasStringCppImpl>(it_path),
                    reinterpret_cast<const char*>(u8_plugin_path.c_str()));

                failed_plugin.AddPluginTo(map);
                result = DAS_S_FALSE;
                continue;
            }

            ForeignLanguageRuntimeFactoryDesc desc{};
            desc.language = up_plugin_desc->language;
            const auto expected_p_runtime = CreateForeignLanguageRuntime(desc);
            if (!expected_p_runtime)
            {
                plugin_create_result = expected_p_runtime.error();

                DAS_CORE_LOG_ERROR(
                    "Error happened when calling CreateForeignLanguageRuntime.\n"
                    "----ForeignLanguageRuntimeFactoryDesc dump begin----");
                DAS_CORE_LOG_ERROR("{{\n{}\n}}", *up_plugin_desc);
                DAS_CORE_LOG_ERROR(
                    "----ForeignLanguageRuntimeFactoryDesc dump end----");

                Details::FailedPluginProxy failed_plugin{
                    plugin_name.Get(),
                    plugin_create_result};
                failed_plugin.AddPluginTo(map);
                result = DAS_S_FALSE;
                continue;
            }
            auto& runtime = expected_p_runtime.value();

            auto expected_p_plugin = runtime->LoadPlugin(plugin_path);
            if (!expected_p_plugin)
            {
                plugin_create_result = expected_p_plugin.error();

                Details::FailedPluginProxy failed_plugin{
                    plugin_name.Get(),
                    plugin_create_result};
                failed_plugin.AddPluginTo(map);
                result = DAS_S_FALSE;
                continue;
            }

            // ! 注意：这里是堆内存，且没有对name进写入，否则会出现问题
            const auto p_u8_plugin_name = up_plugin_desc->name.c_str();
            auto       plugin = Plugin{
                runtime,
                expected_p_plugin.value(),
                std::move(up_plugin_desc)};
            AddInterface(plugin, p_u8_plugin_name);

            map.emplace(plugin_name, std::move(plugin));
        }
    }

    name_plugin_map_ = std::move(map);
    is_inited_ = true;
    return result;
}

DasResult PluginManager::GetErrorMessage(
    const DasGuid&       iid,
    DasResult            error_code,
    IDasReadOnlyString** pp_out_error_message)
{
    DAS_UTILS_CHECK_POINTER(pp_out_error_message)

    if (IsUnexpectedThread())
    {
        return DAS_E_UNEXPECTED_THREAD_DETECTED;
    }

    std::lock_guard _{mutex_};

    DasResult result{DAS_E_UNDEFINED_RETURN_VALUE};

    DasPtr<IDasReadOnlyString> p_default_locale_name{};
    ::DasGetDefaultLocale(p_default_locale_name.Put());

    error_lens_manager_
        .GetErrorMessage(iid, p_default_locale_name.Get(), error_code)
        .map(
            [&result, pp_out_error_message](const auto& p_error_message)
            {
                p_error_message->AddRef();
                *pp_out_error_message = p_error_message.Get();
                result = DAS_S_OK;
            })
        .or_else([&result](const auto ec) { result = ec; });

    return result;
}

bool PluginManager::IsInited() const noexcept
{
    std::lock_guard _{mutex_};
    return is_inited_;
}

DasResult PluginManager::GetAllPluginInfo(
    IDasPluginInfoVector** pp_out_plugin_info_vector)
{
    DAS_UTILS_CHECK_POINTER(pp_out_plugin_info_vector)

    if (IsUnexpectedThread())
    {
        return DAS_E_UNEXPECTED_THREAD_DETECTED;
    }

    std::lock_guard _{mutex_};

    const auto p_vector = MakeDasPtr<DasPluginInfoVectorImpl>();
    for (const auto& pair : name_plugin_map_)
    {
        const auto& plugin_desc = pair.second;
        p_vector->AddInfo(plugin_desc.GetInfo());
    }

    auto& p_out_plugin_info_vector = *pp_out_plugin_info_vector;
    p_out_plugin_info_vector = *p_vector.Get();
    p_out_plugin_info_vector->AddRef();

    return DAS_S_OK;
}

auto PluginManager::GetInterfaceStaticStorage(IDasTypeInfo* p_type_info) const
    -> DAS::Utils::Expected<
        std::reference_wrapper<const InterfaceStaticStorage>>
{
    if (p_type_info == nullptr)
    {
        DAS_CORE_LOG_ERROR("Null pointer found. Please check your code.");
        return tl::make_unexpected(DAS_E_INVALID_POINTER);
    }

    if (IsUnexpectedThread())
    {
        return tl::make_unexpected(DAS_E_UNEXPECTED_THREAD_DETECTED);
    }

    std::lock_guard _{mutex_};

    DasGuid guid;
    if (const auto gg_result = p_type_info->GetGuid(&guid); IsFailed(gg_result))
    {
        DAS_CORE_LOG_ERROR(
            "Get GUID failed. Pointer = {}",
            Utils::VoidP(p_type_info));
        return tl::make_unexpected(gg_result);
    }

    return Details::GetInterfaceStaticStorage(guid_storage_map_, guid);
}

auto PluginManager::GetInterfaceStaticStorage(IDasSwigTypeInfo* p_type_info)
    const -> DAS::Utils::Expected<
              std::reference_wrapper<const InterfaceStaticStorage>>
{
    if (p_type_info == nullptr)
    {
        DAS_CORE_LOG_ERROR("Null pointer found. Please check your code.");
        return tl::make_unexpected(DAS_E_INVALID_POINTER);
    }

    if (IsUnexpectedThread())
    {
        return tl::make_unexpected(DAS_E_UNEXPECTED_THREAD_DETECTED);
    }

    std::lock_guard _{mutex_};

    const auto gg_result = p_type_info->GetGuid();
    if (IsFailed(gg_result.error_code))
    {
        DAS_CORE_LOG_ERROR(
            "Get GUID failed. Pointer = {}",
            Utils::VoidP(p_type_info));
        return tl::make_unexpected(gg_result.error_code);
    }

    return Details::GetInterfaceStaticStorage(
        guid_storage_map_,
        gg_result.value);
}

DAS_NS_ANONYMOUS_DETAILS_BEGIN

bool CheckIsFindInterfaceResultSucceed(
    const DasResult error_code,
    const char*     variable_name)
{
    if (IsFailed(error_code))
    {
        if (error_code != DAS_E_NO_INTERFACE)
        {
            DAS_CORE_LOG_ERROR(
                "Error happened. Code = {}. Variable name = {}",
                error_code,
                variable_name);
            return false;
        }
        DAS_CORE_LOG_INFO("Interface not found in {}", variable_name);
        return false;
    }
    return true;
}

DAS_NS_ANONYMOUS_DETAILS_END

DasResult PluginManager::FindInterface(const DasGuid& iid, void** pp_out_object)
{
    if (IsUnexpectedThread())
    {
        return DAS_E_UNEXPECTED_THREAD_DETECTED;
    }

    std::lock_guard _{mutex_};

    DasResult result{};

    result = task_manager_.FindInterface(
        iid,
        reinterpret_cast<IDasTask**>(pp_out_object));

    if (Details::CheckIsFindInterfaceResultSucceed(result, "task_manager_"))
    {
        return result;
    }

    if (const auto factory_it = std::find_if(
            DAS_FULL_RANGE_OF(capture_factory_vector_),
            [&iid](const DasPtr<IDasCaptureFactory>& p_factory)
            {
                try
                {
                    const auto factory_iid =
                        Utils::GetGuidFrom(p_factory.Get());
                    return factory_iid == iid;
                }
                catch (const DasException& ex)
                {
                    DAS_CORE_LOG_EXCEPTION(ex);
                    return false;
                }
            });
        factory_it != capture_factory_vector_.end())
    {
        *pp_out_object = factory_it->Get();
        factory_it->Get()->AddRef();
        return DAS_S_OK;
    }

    result = error_lens_manager_.FindInterface(
        iid,
        reinterpret_cast<IDasErrorLens**>(pp_out_object));
    if (Details::CheckIsFindInterfaceResultSucceed(
            result,
            "error_lens_manager_"))
    {
        return result;
    }

    result = input_factory_manager_.FindInterface(
        iid,
        reinterpret_cast<IDasInputFactory**>(pp_out_object));
    if (Details::CheckIsFindInterfaceResultSucceed(
            result,
            "input_factory_manager_"))
    {
        return result;
    }

    return DAS_E_NO_INTERFACE;
}

DasResult PluginManager::CreateCaptureManager(
    IDasReadOnlyString*  environment_config,
    IDasCaptureManager** pp_out_manager)
{
    DAS_UTILS_CHECK_POINTER(environment_config)
    DAS_UTILS_CHECK_POINTER(pp_out_manager)

    if (IsUnexpectedThread())
    {
        return DAS_E_UNEXPECTED_THREAD_DETECTED;
    }

    std::lock_guard _{mutex_};

    auto [error_code, p_capture_manager_impl] = CreateDasCaptureManagerImpl(
        capture_factory_vector_,
        environment_config,
        *this);

    if (DAS::IsFailed(error_code))
    {
        return error_code;
    }

    IDasCaptureManager* p_result =
        static_cast<decltype(p_result)>(*p_capture_manager_impl);
    p_result->AddRef();
    *pp_out_manager = p_result;
    return error_code;
}

DasRetCaptureManager PluginManager::CreateCaptureManager(
    DasReadOnlyString environment_config)
{
    DasRetCaptureManager result{};
    auto* const          p_json_config = environment_config.Get();

    auto [error_code, p_capture_manager_impl] = CreateDasCaptureManagerImpl(
        capture_factory_vector_,
        p_json_config,
        *this);

    result.error_code = error_code;
    if (DAS::IsFailed(result.error_code))
    {
        return result;
    }

    result.SetValue(
        static_cast<IDasSwigCaptureManager*>(*p_capture_manager_impl));
    return result;
}

DasResult PluginManager::CreateComponent(
    const DasGuid&  iid,
    IDasComponent** pp_out_component)
{
    if (IsUnexpectedThread())
    {
        return DAS_E_UNEXPECTED_THREAD_DETECTED;
    }

    std::lock_guard _{mutex_};

    const auto result =
        component_factory_manager_.CreateObject(iid, pp_out_component);

    if (Details::CheckIsFindInterfaceResultSucceed(
            result,
            "component_factory_manager_"))
    {
        return result;
    }
    return result;
}

DasRetComponent PluginManager::CreateComponent(const DasGuid& iid)
{
    if (IsUnexpectedThread())
    {
        return {DAS_E_UNEXPECTED_THREAD_DETECTED};
    }

    std::lock_guard _{mutex_};

    return component_factory_manager_.CreateObject(iid);
};

DasResult PluginManager::GetPluginSettingsJson(
    const DasGuid&       plugin_guid,
    IDasReadOnlyString** pp_out_json)
{
    std::lock_guard l{mutex_};
    for (const auto& [_, v] : name_plugin_map_)
    {
        if (v.sp_desc_->guid == plugin_guid)
        {
            v.sp_desc_->settings_json_->GetValue(pp_out_json);
            return DAS_S_OK;
        }
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult PluginManager::SetPluginSettingsJson(
    const DasGuid&      plugin_guid,
    IDasReadOnlyString* p_json)
{
    std::lock_guard l{mutex_};
    for (const auto& [_, v] : name_plugin_map_)
    {
        if (v.sp_desc_->guid == plugin_guid)
        {
            // 注意：这里的值就IPluginInfo中的值，因此设置后调度器可以直接拿到
            v.sp_desc_->settings_json_->SetValue(p_json);
            v.sp_desc_->on_settings_changed(v.sp_desc_->settings_json_);
            return DAS_S_OK;
        }
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult PluginManager::ResetPluginSettings(const DasGuid& plugin_guid)
{
    std::lock_guard l{mutex_};
    for (const auto& [_, v] : name_plugin_map_)
    {
        if (v.sp_desc_->guid == plugin_guid)
        {
            DasReadOnlyStringWrapper default_settings{
                v.sp_desc_->default_settings.dump()};
            v.sp_desc_->settings_json_->SetValue(default_settings.Get());
            return DAS_S_OK;
        }
    }
    return DAS_E_OUT_OF_RANGE;
}

auto PluginManager::FindInterfaceStaticStorage(DasGuid iid)
    -> Utils::Expected<std::reference_wrapper<const InterfaceStaticStorage>>
{
    if (const auto it = guid_storage_map_.find(iid);
        it != guid_storage_map_.end())
    {
        return std::cref(it->second);
    }
    return tl::make_unexpected(DAS_E_OUT_OF_RANGE);
}

DAS_NS_ANONYMOUS_DETAILS_BEGIN

DAS_INTERFACE IInternalIDasInitializeIDasPluginManagerWaiter
{
    virtual DasResult Wait() = 0;
    virtual ~IInternalIDasInitializeIDasPluginManagerWaiter() = default;
};

template <class Work>
class InternalIDasInitializeIDasPluginManagerWaiterImpl final
    : public IInternalIDasInitializeIDasPluginManagerWaiter
{
    Work                                            work_;
    DasPtr<IDasInitializeIDasPluginManagerCallback> p_on_finished_;

    DasResult Wait() override
    {
        DAS_CORE_LOG_INFO("Waiting...");
        const auto [initialize_result] = stdexec::sync_wait(work_).value();
        if (p_on_finished_)
        {
            DAS_CORE_LOG_INFO(
                "Calling IDasInitializeIDasPluginManagerCallback::OnFinished()!");
            const auto result = p_on_finished_->OnFinished(initialize_result);
            DAS_CORE_LOG_INFO("Call finished. Result = {}.", result);
            return result;
        }

        DAS_CORE_LOG_INFO("No callback found.");
        return initialize_result;
    }

public:
    InternalIDasInitializeIDasPluginManagerWaiterImpl(
        Work&&                                   work,
        IDasInitializeIDasPluginManagerCallback* p_on_finished)
        : work_{std::move(work)}, p_on_finished_{p_on_finished}
    {
    }
    ~InternalIDasInitializeIDasPluginManagerWaiterImpl() override = default;
};

class IDasInitializeIDasPluginManagerWaiterImpl final
    : public IDasInitializeIDasPluginManagerWaiter
{
    std::unique_ptr<IInternalIDasInitializeIDasPluginManagerWaiter> up_waiter_;
    DAS_UTILS_IDASBASE_AUTO_IMPL(IDasInitializeIDasPluginManagerWaiterImpl)
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override
    {
        return Utils::QueryInterface<IDasInitializeIDasPluginManagerWaiter>(
            this,
            iid,
            pp_object);
    }
    DasResult Wait() override { return up_waiter_->Wait(); }

public:
    IDasInitializeIDasPluginManagerWaiterImpl(
        IDasReadOnlyGuidVector*                  p_ignore_plugins_guid,
        IDasInitializeIDasPluginManagerCallback* p_on_finished)
        : up_waiter_{new InternalIDasInitializeIDasPluginManagerWaiterImpl{
              stdexec::when_all(stdexec::start_on(
                  DAS::Core::g_scheduler.GetSchedulerImpl(),
                  stdexec::just(DasPtr{p_ignore_plugins_guid})
                      | stdexec::then(
                          [](DasPtr<IDasReadOnlyGuidVector> p_vector)
                          {
                              DAS_CORE_LOG_INFO(
                                  "Initializing plugin manager...");
                              auto& plugin_manager = DAS::Core::
                                  ForeignInterfaceHost::g_plugin_manager;
                              plugin_manager.UpdateBindingThread();
                              return plugin_manager.Refresh(p_vector.Get());
                          }))),
              p_on_finished}}
    {
    }
    ~IDasInitializeIDasPluginManagerWaiterImpl() = default;
};

DAS_NS_ANONYMOUS_DETAILS_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

DasResult InitializeIDasPluginManager(
    IDasReadOnlyGuidVector*                  p_ignore_plugins_guid,
    IDasInitializeIDasPluginManagerCallback* p_on_finished,
    IDasInitializeIDasPluginManagerWaiter**  pp_out_waiter)
{
    DAS_UTILS_CHECK_POINTER(p_ignore_plugins_guid)

    static size_t initialize_counter{0};
    ++initialize_counter;
    if (initialize_counter > 1)
    {
        DAS_CORE_LOG_ERROR(
            "The plugin should be loaded only once while the program is running. initialize_counter = {}.",
            initialize_counter);
        if (DAS::Core::ForeignInterfaceHost::g_plugin_manager
                .IsUnexpectedThread())
        {
            std::stringstream ss;
            ss << std::this_thread::get_id();
            DAS_CORE_LOG_ERROR(
                "Try to initialize the plugin manager on an unexpected thread. Id = {}.",
                ss.str());
            return DAS_E_UNEXPECTED_THREAD_DETECTED;
        }
    }
    else
    {
        if (pp_out_waiter)
        {
            const auto p_waiter = DAS::MakeDasPtr<
                IDasInitializeIDasPluginManagerWaiter,
                DAS::Core::ForeignInterfaceHost::Details::
                    IDasInitializeIDasPluginManagerWaiterImpl>(
                p_ignore_plugins_guid,
                p_on_finished);
            DAS::Utils::SetResult(p_waiter.Get(), pp_out_waiter);
        }
    }

    return DAS_S_OK;
}

DasResult CreateIDasPluginManagerAndGetResult(
    IDasReadOnlyGuidVector* p_ignore_plugins_guid,
    IDasPluginManager**     pp_out_result)
{
    DAS_UTILS_CHECK_POINTER(p_ignore_plugins_guid)
    DAS_UTILS_CHECK_POINTER(pp_out_result)

    auto& plugin_manager = DAS::Core::ForeignInterfaceHost::g_plugin_manager;
    if (plugin_manager.IsUnexpectedThread())
    {
        std::stringstream ss;
        ss << std::this_thread::get_id();
        DAS_CORE_LOG_ERROR(
            "Try to create the plugin manager on an unexpected thread. Id = {}.",
            ss.str());
        return DAS_E_UNEXPECTED_THREAD_DETECTED;
    }
    const auto result = plugin_manager.Refresh(p_ignore_plugins_guid);
    *pp_out_result = plugin_manager;
    return result;
}

DasResult GetExistingIDasPluginManager(IDasPluginManager** pp_out_result)
{
    DAS_UTILS_CHECK_POINTER(pp_out_result)
    auto& plugin_manager = DAS::Core::ForeignInterfaceHost::g_plugin_manager;
    if (plugin_manager.IsUnexpectedThread())
    {
        std::stringstream ss;
        ss << std::this_thread::get_id();
        DAS_CORE_LOG_ERROR(
            "Try to get existing plugin manager on an unexpected thread. Id = {}.",
            ss.str());
        return DAS_E_UNEXPECTED_THREAD_DETECTED;
    }
    if (plugin_manager.IsInited())
    {
        DAS::Utils::SetResult(plugin_manager, pp_out_result);
        return DAS_S_OK;
    }
    return DAS_E_OBJECT_NOT_INIT;
}

DasRetPluginManager GetExistingIDasPluginManager()
{
    auto& plugin_manager = DAS::Core::ForeignInterfaceHost::g_plugin_manager;
    if (plugin_manager.IsUnexpectedThread())
    {
        std::stringstream ss;
        ss << std::this_thread::get_id();
        DAS_CORE_LOG_ERROR(
            "Try to get existing plugin manager on an unexpected thread. Id = {}.",
            ss.str());
        return {DAS_E_UNEXPECTED_THREAD_DETECTED};
    }
    return plugin_manager.IsInited()
               ? DasRetPluginManager{DAS_S_OK, static_cast<IDasSwigPluginManager*>(Das::Core::ForeignInterfaceHost::g_plugin_manager)}
               : DasRetPluginManager{DAS_E_OBJECT_NOT_INIT};
}