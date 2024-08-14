#include "AutoStarRail/Core/Exceptions/PythonException.h"
#include "AutoStarRail/ExportInterface/IAsrPluginManager.h"

#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/Core/ForeignInterfaceHost/AsrGuid.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/AsrStringImpl.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/IAsrPluginManagerImpl.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/PluginManager.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/Core/Utils/InternalUtils.h>
#include <AutoStarRail/Core/i18n/AsrResultTranslator.h>
#include <AutoStarRail/Core/i18n/GlobalLocale.h>
#include <AutoStarRail/IAsrBase.h>
#include <AutoStarRail/PluginInterface/IAsrErrorLens.h>
#include <AutoStarRail/PluginInterface/IAsrTask.h>
#include <AutoStarRail/Utils/CommonUtils.hpp>
#include <AutoStarRail/Utils/Expected.h>
#include <AutoStarRail/Utils/StreamUtils.hpp>
#include <AutoStarRail/Utils/StringUtils.h>
#include <AutoStarRail/Utils/UnexpectedEnumException.h>
#include <algorithm>
#include <boost/pfr/core.hpp>
#include <fstream>
#include <functional>
#include <magic_enum.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <utility>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

using CommonBasePtr = std::variant<AsrPtr<IAsrBase>, AsrPtr<IAsrSwigBase>>;

ASR_NS_ANONYMOUS_DETAILS_BEGIN

auto GetFeaturesFrom(const CommonPluginPtr& p_plugin)
    -> ASR::Utils::Expected<std::vector<AsrPluginFeature>>
{
    constexpr size_t MAX_ENUM_COUNT =
        magic_enum::enum_count<AsrPluginFeature>() + static_cast<size_t>(30);

    AsrPluginFeature feature{};
    auto             result = std::vector<AsrPluginFeature>{0};
    result.reserve(magic_enum::enum_count<AsrPluginFeature>());

    for (size_t i = 0; i < MAX_ENUM_COUNT; ++i)
    {
        const auto get_feature_result =
            CommonPluginEnumFeature(p_plugin, i, &feature);
        if (IsOk(get_feature_result))
        {
            result.push_back(feature);
            continue;
        }
        if (get_feature_result == ASR_E_OUT_OF_RANGE)
        {
            return result;
        }
        return tl::make_unexpected(get_feature_result);
    }
    ASR_CORE_LOG_WARN(
        "Executing function \"EnumFeature\" in plugin more than the maximum limit of {} times, stopping.",
        MAX_ENUM_COUNT);
    return result;
}

template <class T>
auto GetSupportedInterface(
    T&                                   p_plugin,
    const std::vector<AsrPluginFeature>& features)
{
    for (auto feature : features)
    {
        AsrPtr<IAsrBase> p_interface{};
        auto             get_interface_result =
            p_plugin->GetInterface(feature, &p_interface);
        if (!ASR::IsOk(get_interface_result))
        {
            // TODO: Call internal error lens to interpret the error code.
            ASR_CORE_LOG_ERROR(
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
 * @param callback The function who return struct { AsrResult error_code,
 * AsrReadOnlyString value; };
 * @return
 */
template <class F>
auto GetErrorMessageAndAutoFallBack(
    const AsrResult     error_code,
    IAsrReadOnlyString* p_locale_name,
    F&& callback) -> ASR::Utils::Expected<ASR::AsrPtr<IAsrReadOnlyString>>
{
    constexpr size_t ERROR_CODE_INDEX = 0;
    constexpr size_t VALUE_INDEX = 1;

    ASR::AsrPtr<IAsrReadOnlyString> result{};
    auto&&     get_error_message = std::forward<F>(callback);
    const auto result_1 = get_error_message(error_code, p_locale_name);
    const auto result_1_error_code =
        boost::pfr::get<ERROR_CODE_INDEX>(result_1);

    if (ASR::IsOk(result_1_error_code))
    {
        // use default locale and retry.
        if (result_1_error_code == ASR_E_OUT_OF_RANGE)
        {
            const auto p_fallback_locale_name =
                ASR::Core::i18n::GetFallbackLocale();
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
    const AsrResult     error_code,
    IAsrReadOnlyString* p_locale_name)
    -> ASR::Utils::Expected<ASR::AsrPtr<IAsrReadOnlyString>>
{
    // 不是插件自定义错误时
    if (error_code < ASR_E_RESERVED)
    {
        return ASR::Core::ForeignInterfaceHost::Details::
            GetErrorMessageAndAutoFallBack(
                error_code,
                p_locale_name,
                [](auto&& internal_error_code, auto&& internal_p_locale_name)
                {
                    ASR::AsrPtr<IAsrReadOnlyString> p_result{};
                    AsrRetReadOnlyString            result{};
                    result.error_code = ASR::Core::i18n::TranslateError(
                        internal_p_locale_name,
                        internal_error_code,
                        p_result.Put());
                    result.value = AsrReadOnlyString{p_result};
                    return result;
                });
    }
    ASR_CORE_LOG_ERROR(
        "The error code {} ({} >= " ASR_STR(
            ASR_E_RESERVED) ") which is not a predefined error code.",
        error_code,
        error_code);
    return tl::make_unexpected(ASR_E_OUT_OF_RANGE);
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
        ASR::Utils::overload_set{
            [index, u8_plugin_name](
                AsrPtr<IAsrPlugin> p_plugin) -> std::optional<CommonBasePtr>
            {
                AsrPtr<IAsrBase> result{};
                if (const auto cfi_result = p_plugin->CreateFeatureInterface(
                        index,
                        result.PutVoid());
                    IsFailed(cfi_result))
                {
                    ASR_CORE_LOG_ERROR(
                        CREATE_FEATURE_INTERFACE_FAILED_MESSAGE,
                        cfi_result,
                        u8_plugin_name);
                    return {};
                }
                return result;
            },
            [index, u8_plugin_name](
                AsrPtr<IAsrSwigPlugin> p_plugin) -> std::optional<CommonBasePtr>
            {
                AsrRetSwigBase cfi_result{};
                cfi_result = p_plugin->CreateFeatureInterface(index);
                if (IsFailed(cfi_result.error_code))
                {
                    ASR_CORE_LOG_ERROR(
                        CREATE_FEATURE_INTERFACE_FAILED_MESSAGE,
                        cfi_result.error_code,
                        u8_plugin_name);
                    return {};
                }
                AsrPtr<IAsrSwigBase> result{reinterpret_cast<IAsrSwigBase*>(
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
        "Failed when querying IAsrTypeInfo or IAsrSwigTypeInfo. ErrorCode = {}. Pointer = {}. Plugin name = {}.";
    return std::visit(
        ASR::Utils::overload_set{
            [p_plugin_name](const AsrPtr<IAsrBase>& p_base)
                -> std::optional<CommonTypeInfoPtr>
            {
                AsrPtr<IAsrTypeInfo> result;
                if (const auto qi_result = p_base.As(result);
                    IsFailed(qi_result))
                {
                    ASR_CORE_LOG_ERROR(
                        QUERY_TYPE_INFO_FAILED_MESSAGE,
                        qi_result,
                        static_cast<void*>(p_base.Get()),
                        p_plugin_name);
                    return {};
                }
                return result;
            },
            [p_plugin_name](const AsrPtr<IAsrSwigBase>& p_base)
                -> std::optional<CommonTypeInfoPtr>
            {
                const auto qi_result =
                    p_base->QueryInterface(AsrIidOf<IAsrSwigTypeInfo>());
                if (IsFailed(qi_result.error_code))
                {
                    ASR_CORE_LOG_ERROR(
                        QUERY_TYPE_INFO_FAILED_MESSAGE,
                        qi_result.error_code,
                        static_cast<void*>(p_base.Get()),
                        p_plugin_name);
                    return {};
                }
                AsrPtr<IAsrSwigTypeInfo> result{static_cast<IAsrSwigTypeInfo*>(
                    qi_result.GetVoidNoAddRef())};
                return result;
            }},
        common_p_base);
}

constexpr auto& GET_GUID_FAILED_MESSAGE =
    "Get guid from interface failed. Error code = {}";

auto GetInterfaceStaticStorage(
    const PluginManager::InterfaceStaticStorageMap& guid_storage_map,
    const AsrGuid&                                  guid)
    -> ASR::Utils::Expected<
        std::reference_wrapper<const PluginManager::InterfaceStaticStorage>>
{
    const auto it_storage = guid_storage_map.find(guid);
    if (it_storage == guid_storage_map.end())
    {
        ASR_CORE_LOG_ERROR(
            "No vaild interface static storage foud. Guid = {}.",
            guid);
    }

    return std::cref(it_storage->second);
}

ASR_NS_ANONYMOUS_DETAILS_END

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

AsrResult AsrGetErrorMessage(
    IAsrTypeInfo*        p_error_generator,
    AsrResult            error_code,
    IAsrReadOnlyString** pp_out_error_message)
{
    AsrGuid guid;
    if (const auto get_guid_result = p_error_generator->GetGuid(&guid);
        ASR::IsFailed(get_guid_result))
    {
        return get_guid_result;
    }

    return Asr::Core::ForeignInterfaceHost::g_plugin_manager.GetErrorMessage(
        guid,
        error_code,
        pp_out_error_message);
}

AsrResult AsrGetPredefinedErrorMessage(
    AsrResult            error_code,
    IAsrReadOnlyString** pp_out_error_message)
{
    ASR::AsrPtr<IAsrReadOnlyString> p_default_locale_name{};
    std::ignore = ::AsrGetDefaultLocale(p_default_locale_name.Put());
    const auto result =
        ASR::Core::ForeignInterfaceHost::Details::GetPredefinedErrorMessage(
            error_code,
            p_default_locale_name.Get());
    if (result)
    {
        auto* const p_result = result.value().Get();
        p_result->AddRef();
        *pp_out_error_message = p_result;
        return ASR_S_OK;
    }
    return result.error();
}

AsrRetReadOnlyString AsrGetErrorMessage(
    IAsrSwigTypeInfo* p_error_generator,
    AsrResult         error_code)
{
    AsrRetReadOnlyString            result{};
    ASR::AsrPtr<IAsrReadOnlyString> p_result;

    const auto ret_guid = p_error_generator->GetGuid();
    if (ASR::IsFailed(ret_guid.error_code))
    {
        result.error_code = ret_guid.error_code;
    }
    result.error_code =
        ASR::Core::ForeignInterfaceHost::g_plugin_manager.GetErrorMessage(
            ret_guid.value,
            error_code,
            p_result.Put());

    return result;
}

AsrRetReadOnlyString AsrGetPredefinedErrorMessage(AsrResult error_code)
{
    AsrRetReadOnlyString            result{};
    ASR::AsrPtr<IAsrReadOnlyString> p_result{};
    result.error_code =
        AsrGetPredefinedErrorMessage(error_code, p_result.Put());
    if (ASR::IsOk(result.error_code))
    {
        result.value = p_result;
    }
    return result;
}

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

ASR_DEFINE_VARIABLE(g_plugin_manager){};

ASR_NS_ANONYMOUS_DETAILS_BEGIN

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
    const char           (&error_message)[N],
    const char*          u8_plugin_name,
    const CommonBasePtr& common_p_base) -> ASR::Utils::Expected<AsrPtr<T>>
{
    using RetType = ASR::Utils::Expected<AsrPtr<T>>;

    return std::visit(
        ASR::Utils::overload_set{
            [&error_message,
             u8_plugin_name](const AsrPtr<IAsrBase>& p_base) -> RetType
            {
                AsrPtr<T> result;
                if (const auto qi_result = p_base.As(result);
                    ASR::IsFailed(qi_result))
                {
                    ASR_CORE_LOG_ERROR(
                        error_message,
                        qi_result,
                        static_cast<void*>(p_base.Get()),
                        u8_plugin_name);
                    return tl::make_unexpected(qi_result);
                }

                return result;
            },
            [&error_message,
             u8_plugin_name](const AsrPtr<IAsrSwigBase>& p_base) -> RetType
            {
                auto qi_result = p_base->QueryInterface(AsrIidOf<SwigT>());
                if (ASR::IsFailed(qi_result.error_code))
                {
                    ASR_CORE_LOG_ERROR(
                        error_message,
                        qi_result.error_code,
                        static_cast<void*>(p_base.Get()),
                        u8_plugin_name);
                    return tl::make_unexpected(qi_result.error_code);
                }

                auto result = MakeAsrPtr<SwigToCpp<SwigT>>(
                    static_cast<IAsrSwigErrorLens*>(
                        qi_result.GetVoidNoAddRef()));

                qi_result.value.Get()->Release();

                return result;
            }},
        common_p_base);
}

auto QueryErrorLensFrom(
    const char*          u8_plugin_name,
    const CommonBasePtr& common_p_base)
    -> ASR::Utils::Expected<AsrPtr<IAsrErrorLens>>
{
    using RetType = ASR::Utils::Expected<AsrPtr<IAsrErrorLens>>;

    constexpr const auto& QUERY_ERROR_LENS_FAILED_MESSAGE =
        "Failed when calling QueryInterface. ErrorCode = {}. Pointer = {}. Plugin name = {}.";

    return std::visit(
        ASR::Utils::overload_set{
            [u8_plugin_name](const AsrPtr<IAsrBase>& p_base) -> RetType
            {
                AsrPtr<IAsrErrorLens> result;
                if (const auto qi_result = p_base.As(result);
                    ASR::IsFailed(qi_result))
                {
                    ASR_CORE_LOG_ERROR(
                        QUERY_ERROR_LENS_FAILED_MESSAGE,
                        qi_result,
                        static_cast<void*>(p_base.Get()),
                        u8_plugin_name);
                    return tl::make_unexpected(qi_result);
                }

                return result;
            },
            [u8_plugin_name](const AsrPtr<IAsrSwigBase>& p_base) -> RetType
            {
                auto qi_result =
                    p_base->QueryInterface(AsrIidOf<IAsrSwigErrorLens>());
                if (ASR::IsFailed(qi_result.error_code))
                {
                    ASR_CORE_LOG_ERROR(
                        QUERY_ERROR_LENS_FAILED_MESSAGE,
                        qi_result.error_code,
                        static_cast<void*>(p_base.Get()),
                        u8_plugin_name);
                    return tl::make_unexpected(qi_result.error_code);
                }

                AsrPtr<IAsrErrorLens> result{new SwigToCpp<IAsrSwigErrorLens>{
                    static_cast<IAsrSwigErrorLens*>(
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
    GetInterfaceFromPluginParam param) -> AsrResult
{
    const auto& [u8_plugin_name, common_p_base] = param;

    AsrPtr<IAsrReadOnlyGuidVector> p_guid_vector{};
    const auto                     expected_p_error_lens =
        QueryErrorLensFrom(u8_plugin_name, common_p_base);
    if (!expected_p_error_lens)
    {
        return expected_p_error_lens.error();
    }

    const auto& p_error_lens = expected_p_error_lens.value();

    if (const auto get_iids_result =
            p_error_lens->GetSupportedIids(p_guid_vector.Put());
        ASR::IsFailed(get_iids_result))
    {
        ASR_CORE_LOG_ERROR(
            "Try to get supported iids failed. Error code = {}. Plugin name = {}.",
            get_iids_result,
            u8_plugin_name);
        return get_iids_result;
    }
    error_lens_manager.Register(p_guid_vector.Get(), p_error_lens.Get());
    return ASR_S_OK;
}

template <class T>
auto RegisterTaskFromPlugin(T& task_manager, GetInterfaceFromPluginParam param)
    -> AsrResult
{
    const auto& [u8_plugin_name, common_p_base] = param;

    using CommonTaskPointer =
        std::variant<AsrPtr<IAsrTask>, AsrPtr<IAsrSwigTask>>;
    using ExpectedCommonTaskPointer = ASR::Utils::Expected<CommonTaskPointer>;

    ExpectedCommonTaskPointer expected_common_p_task = std::visit(
        ASR::Utils::overload_set{
            [](const AsrPtr<IAsrBase>& p_base) -> ExpectedCommonTaskPointer
            {
                AsrPtr<IAsrTask> p_task{};
                if (const auto qi_result = p_base.As(p_task);
                    IsFailed(qi_result))
                {
                    return tl::make_unexpected(qi_result);
                }
                return p_task;
            },
            [](const AsrPtr<IAsrSwigBase>& p_base) -> ExpectedCommonTaskPointer
            {
                try
                {
                    const auto qi_result =
                        p_base->QueryInterface(AsrIidOf<IAsrSwigTask>());
                    if (IsFailed(qi_result.error_code))
                    {
                        return tl::make_unexpected(qi_result.error_code);
                    }
                    return AsrPtr{static_cast<IAsrSwigTask*>(
                        qi_result.GetVoidNoAddRef())};
                }
                catch (...)
                {
                    return tl::make_unexpected(ASR_E_INTERNAL_FATAL_ERROR);
                }
            }},
        common_p_base);
    if (!expected_common_p_task)
    {
        return expected_common_p_task.error();
    }

    return std::visit(
        ASR::Utils::overload_set{
            [u8_plugin_name, &task_manager](const AsrPtr<IAsrTask>& p_task)
            {
                try
                {
                    const auto guid = Utils::GetGuidFrom(p_task.Get());
                    return task_manager.Register(p_task.Get(), guid);
                }
                catch (const AsrException& ex)
                {
                    ASR_CORE_LOG_ERROR(
                        "Get guid in IAsrTask object failed."
                        "Plugin name = {}. Error Code = {}. Error message = {}.",
                        u8_plugin_name,
                        ex.GetErrorCode(),
                        ex.what());
                    return ex.GetErrorCode();
                }
            },
            [u8_plugin_name, &task_manager](const AsrPtr<IAsrSwigTask>& p_task)
            {
                try
                {
                    const auto guid = Utils::GetGuidFrom(p_task.Get());
                    return task_manager.Register(p_task.Get(), guid);
                }
                catch (const AsrException& ex)
                {
                    ASR_CORE_LOG_ERROR(
                        "Get guid in IAsrSwigTask object failed."
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
    GetInterfaceFromPluginParam param) -> AsrResult
{
    const auto& [u8_plugin_name, common_p_base] = param;

    const auto pp_base = std::get_if<AsrPtr<IAsrBase>>(&common_p_base);
    if (pp_base == nullptr) [[unlikely]]
    {
        ASR_CORE_LOG_ERROR(
            "Variable common_p_base does NOT contain AsrPtr<IAsrBase> object. Plugin name = {}.",
            u8_plugin_name);
        return ASR_E_INTERNAL_FATAL_ERROR;
    }
    const auto&                p_base = *pp_base;
    AsrPtr<IAsrCaptureFactory> p_result{};
    if (const auto qi_result = p_base.As(p_result); IsFailed(qi_result))
    {
        ASR_CORE_LOG_ERROR(
            "Can not convert interface to IAsrCaptureFactory. Plugin name = {}. Error code = {}.",
            u8_plugin_name,
            qi_result);
        return qi_result;
    }
    capture_factory_vector.emplace_back();
    return ASR_S_OK;
}

template <class T>
auto RegisterInputFactoryFromPlugin(
    T&                          input_factory_manager,
    GetInterfaceFromPluginParam param) -> AsrResult
{
    const auto& [u8_plugin_name, common_p_base] = param;

    using CommonInputFactoryPointer =
        std::variant<AsrPtr<IAsrInputFactory>, AsrPtr<IAsrSwigInputFactory>>;
    using ExpectedCommonInputFactoryPointer =
        ASR::Utils::Expected<CommonInputFactoryPointer>;

    ExpectedCommonInputFactoryPointer expected_common_p_input_factory =
        std::visit(
            ASR::Utils::overload_set{
                [](const AsrPtr<IAsrBase>& p_base)
                    -> ExpectedCommonInputFactoryPointer
                {
                    AsrPtr<IAsrInputFactory> p_task{};
                    if (const auto qi_result = p_base.As(p_task);
                        IsFailed(qi_result))
                    {
                        return tl::make_unexpected(qi_result);
                    }
                    return p_task;
                },
                [](const AsrPtr<IAsrSwigBase>& p_base)
                    -> ExpectedCommonInputFactoryPointer
                {
                    const auto qi_result = p_base->QueryInterface(
                        AsrIidOf<IAsrSwigInputFactory>());
                    if (IsFailed(qi_result.error_code))
                    {
                        return tl::make_unexpected(qi_result.error_code);
                    }
                    return AsrPtr{static_cast<IAsrSwigInputFactory*>(
                        qi_result.GetVoidNoAddRef())};
                }},
            common_p_base);

    if (!expected_common_p_input_factory)
    {
        return expected_common_p_input_factory.error();
    }

    return std::visit(
        Utils::overload_set{
            [&input_factory_manager](const AsrPtr<IAsrInputFactory>& p_factory)
            { return input_factory_manager.Register(p_factory.Get()); },
            [&input_factory_manager](
                const AsrPtr<IAsrSwigInputFactory>& p_factory)
            { return input_factory_manager.Register(p_factory.Get()); }},
        expected_common_p_input_factory.value());
}

const std::string UPPER_CURRENT_PLATFORM = []
{
    std::string result{ASR_PLATFORM};
    std::transform(
        ASR_FULL_RANGE_OF(result),
        result.begin(),
        [](unsigned char c) { return toupper(c); });
    return result;
}();

ASR_NS_ANONYMOUS_DETAILS_END

IAsrPluginManagerImpl::IAsrPluginManagerImpl(PluginManager& impl) : impl_{impl}
{
}

int64_t IAsrPluginManagerImpl::AddRef() { return impl_.AddRef(); }

int64_t IAsrPluginManagerImpl::Release() { return impl_.Release(); }

AsrResult IAsrPluginManagerImpl::QueryInterface(
    const AsrGuid& iid,
    void**         pp_object)
{
    return Utils::QueryInterface<IAsrPluginManager>(this, iid, pp_object);
}

AsrResult IAsrPluginManagerImpl::GetAllPluginInfo(
    IAsrPluginInfoVector** pp_out_plugin_info_vector)
{
    return impl_.GetAllPluginInfo(pp_out_plugin_info_vector);
}

AsrResult IAsrPluginManagerImpl::FindInterface(
    const AsrGuid& iid,
    void**         pp_object)
{
    return impl_.FindInterface(iid, pp_object);
}

AsrResult PluginManager::AddInterface(
    const Plugin& plugin,
    const char*   u8_plugin_name)
{
    AsrResult   result{ASR_S_OK};
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
                    ASR_FULL_RANGE_OF(relative_path)};
                InterfaceStaticStorage storage{
                    {std::filesystem::current_path() / u8_relative_path},
                    plugin.sp_desc_};
                std::visit(
                    ASR::Utils::overload_set{
                        [this,
                         &storage](const AsrPtr<IAsrTypeInfo>& p_type_info) {
                            RegisterInterfaceStaticStorage(
                                p_type_info.Get(),
                                storage);
                        },
                        [this, &storage](
                            const AsrPtr<IAsrSwigTypeInfo>& p_type_info) {
                            RegisterInterfaceStaticStorage(
                                p_type_info.Get(),
                                storage);
                        }},
                    opt_common_p_type_info.value());
            }
        }

        switch (feature)
        {
        case ASR_PLUGIN_FEATURE_ERROR_LENS:
        {
            if (const auto relfp_result = Details::RegisterErrorLensFromPlugin(
                    error_lens_manager_,
                    {u8_plugin_name, opt_common_p_base.value()});
                IsFailed(relfp_result))
            {
                ASR_CORE_LOG_ERROR(
                    "Can not get error lens interface from plugin {}. "
                    "Error code = {}.",
                    u8_plugin_name,
                    relfp_result);
                result = ASR_S_FALSE;
            }
            break;
        }
        case ASR_PLUGIN_FEATURE_TASK:
        {
            if (const auto rtfp_result = Details::RegisterTaskFromPlugin(
                    task_manager_,
                    {u8_plugin_name, opt_common_p_base.value()});
                IsFailed(rtfp_result))
            {
                ASR_CORE_LOG_ERROR(
                    "Can not get task interface from plugin {}. "
                    "Error code = {}.",
                    u8_plugin_name,
                    rtfp_result);
                result = ASR_S_FALSE;
            }
            break;
        }
        case ASR_PLUGIN_FEATURE_CAPTURE_FACTORY:
        {
            if (const auto rcffp_result =
                    Details::RegisterCaptureFactoryFromPlugin(
                        capture_factory_vector_,
                        {u8_plugin_name, opt_common_p_base.value()});
                IsFailed(rcffp_result))
            {
                ASR_CORE_LOG_ERROR(
                    "Can not get capture factory interface from plugin {}. "
                    "Error code = {}.",
                    u8_plugin_name,
                    rcffp_result);
                result = ASR_S_FALSE;
            }
            break;
        }
        case ASR_PLUGIN_FEATURE_INPUT_FACTORY:
        {
            if (const auto riffp_result =
                    Details::RegisterInputFactoryFromPlugin(
                        input_factory_manager_,
                        {u8_plugin_name, opt_common_p_base.value()});
                IsFailed(riffp_result))
            {
                ASR_CORE_LOG_ERROR(
                    "Can not get input factory interface from plugin {}. "
                    "Error code = {}.",
                    u8_plugin_name,
                    riffp_result);
                result = ASR_S_FALSE;
            }
            break;
        }
        default:
            throw ASR::Utils::UnexpectedEnumException::FromEnum(feature);
        }
    }
    return result;
}

void PluginManager::RegisterInterfaceStaticStorage(
    IAsrTypeInfo*                 p_interface,
    const InterfaceStaticStorage& storage)
{
    AsrGuid guid;
    if (const auto get_guid_result = p_interface->GetGuid(&guid);
        IsFailed(get_guid_result))
    {
        ASR_CORE_LOG_ERROR(Details::GET_GUID_FAILED_MESSAGE, get_guid_result);
    }

    guid_storage_map_[guid] = storage;
}

void PluginManager::RegisterInterfaceStaticStorage(
    IAsrSwigTypeInfo*             p_swig_interface,
    const InterfaceStaticStorage& storage)
{
    const auto ret_guid = p_swig_interface->GetGuid();
    if (IsFailed(ret_guid.error_code))
    {
        ASR_CORE_LOG_ERROR(
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

    ASR::Utils::EnableStreamException(
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

AsrResult PluginManager::GetInterface(const Plugin& plugin)
{
    (void)plugin;
    return ASR_E_NO_IMPLEMENTATION;
}

ASR_NS_ANONYMOUS_DETAILS_BEGIN

/**
 * @brief 插件创建失败时，产生这个类，负责生成报错的信息和错误状态的插件
 */
struct FailedPluginProxy : public ASR::Utils::NonCopyableAndNonMovable
{
    AsrPtr<IAsrReadOnlyString> error_message;
    AsrPtr<IAsrReadOnlyString> name;
    AsrResult                  error_code;

    FailedPluginProxy(
        const std::filesystem::path& metadata_path,
        AsrResult                    error_code)
        : error_message{[](AsrResult error_code)
                        {
                            AsrPtr<IAsrReadOnlyString> result{};
                            AsrGetPredefinedErrorMessage(
                                error_code,
                                result.Put());
                            return result;
                        }(error_code)},
          name{MakeAsrPtr<IAsrReadOnlyString, ::AsrStringCppImpl>(
              metadata_path)},
          error_code{error_code}
    {
    }

    FailedPluginProxy(IAsrReadOnlyString* plugin_name, AsrResult error_code)
        : error_message{[](AsrResult error_code)
                        {
                            AsrPtr<IAsrReadOnlyString> result{};
                            AsrGetPredefinedErrorMessage(
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

ASR_NS_ANONYMOUS_DETAILS_END

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

AsrResult PluginManager::Refresh(IAsrReadOnlyGuidVector* p_ignored_guid_vector)
{
    AsrResult result{ASR_S_OK};

    const auto ignored_guid_set = [p_ignored_guid_vector]
    {
        std::unordered_set<AsrGuid> lambda_result{};
        size_t                      i{0};
        AsrGuid                     guid;
        while (true)
        {
            const auto error_code = p_ignored_guid_vector->At(i, &guid);
            if (IsOk(error_code))
            {
                ++i;
                lambda_result.emplace(guid);
                continue;
            }

            if (error_code != ASR_E_OUT_OF_RANGE)
            {
                ASR_CORE_LOG_ERROR(
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
        if (ASR_UTILS_STRINGUTILS_COMPARE_STRING(extension, ".json"))
        {
            AsrResult plugin_create_result{ASR_E_UNDEFINED_RETURN_VALUE};
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
                    plugin_create_result = ASR_E_UNSUPPORTED_SYSTEM;

                    auto failed_plugin = Details::FailedPluginProxy{
                        it_path,
                        plugin_create_result};

                    ASR_CORE_LOG_ERROR(
                        "Error when checking system requirement. Error code = " ASR_STR(
                            ASR_E_UNSUPPORTED_SYSTEM) ".");
                    // 此处，plugin_name即为metadata路径，见
                    // Details::AddFailedPluginAndReturnTmpPluginName
                    ASR_CORE_LOG_ERROR(
                        "NOTE: plugin meta data file path:\"{}\"",
                        failed_plugin.name);

                    failed_plugin.AddPluginTo(map);
                    result = ASR_S_FALSE;
                    continue;
                }
            }
            catch (const nlohmann::json::exception& ex)
            {
                ASR_CORE_LOG_EXCEPTION(ex);
                ASR_CORE_LOG_INFO(
                    "Error happened when parsing plugin metadata file. Error code = " ASR_STR(
                        ASR_CORE_LOG_INFO) ".");
                plugin_create_result = ASR_E_INVALID_JSON;
            }
            catch (const std::ios_base::failure& ex)
            {
                ASR_CORE_LOG_EXCEPTION(ex);
                ASR_CORE_LOG_INFO(
                    "Error happened when reading plugin metadata file. Error code = " ASR_STR(
                        ASR_E_INVALID_FILE) ".");
                plugin_create_result = ASR_E_INVALID_FILE;
            }

            if (plugin_create_result != ASR_E_UNDEFINED_RETURN_VALUE)
            {
                auto failed_plugin =
                    Details::FailedPluginProxy{it_path, plugin_create_result};
                ASR_CORE_LOG_INFO(
                    "NOTE: plugin meta data file path:\"{}\"",
                    failed_plugin.name);

                failed_plugin.AddPluginTo(map);
                result = ASR_S_FALSE;
                continue;
            }

            AsrPtr<IAsrReadOnlyString> plugin_name{};

            if (auto expected_plugin_name =
                    ASR::Core::Utils::MakeAsrReadOnlyStringFromUtf8(
                        up_plugin_desc->name);
                !expected_plugin_name)
            {
                const auto error_code = expected_plugin_name.error();
                ASR_CORE_LOG_ERROR(
                    "Can not convert std::string to IAsrReadOnlyString when getting plugin name."
                    "\nError code = {}",
                    error_code);
                auto failed_plugin =
                    Details::FailedPluginProxy{it_path, error_code};
                failed_plugin.AddPluginTo(map);
                result = ASR_S_FALSE;
                continue;
            }
            else
            {
                plugin_name = expected_plugin_name.value();
            }

            std::filesystem::path plugin_path =
                it_path.parent_path()
                / std::filesystem::path{
                    std::u8string{ASR_FULL_RANGE_OF(up_plugin_desc->name)}
                    + std::u8string{u8"."}
                    + std::u8string{ASR_FULL_RANGE_OF(
                        up_plugin_desc->plugin_filename_extension)}};
            if (!std::filesystem::exists(plugin_path))
            {
                plugin_create_result = ASR_E_FILE_NOT_FOUND;

                ASR_CORE_LOG_ERROR(
                    "Error when checking plugin file. Error code = " ASR_STR(
                        ASR_E_FILE_NOT_FOUND) ".");

                Details::FailedPluginProxy failed_plugin{
                    plugin_name.Get(),
                    plugin_create_result};

                const auto u8_plugin_path = plugin_path.u8string();
                ASR_CORE_LOG_INFO(
                    "NOTE: Plugin metadata file path:\"{}\". "
                    "Expected plugin file path \"{}\"",
                    MakeAsrPtr<IAsrReadOnlyString, AsrStringCppImpl>(it_path),
                    reinterpret_cast<const char*>(u8_plugin_path.c_str()));

                failed_plugin.AddPluginTo(map);
                result = ASR_S_FALSE;
                continue;
            }

            ForeignLanguageRuntimeFactoryDesc desc{};
            desc.language = up_plugin_desc->language;
            const auto expected_p_runtime = CreateForeignLanguageRuntime(desc);
            if (!expected_p_runtime)
            {
                plugin_create_result = expected_p_runtime.error();

                ASR_CORE_LOG_ERROR(
                    "Error happened when calling CreateForeignLanguageRuntime.\n"
                    "----ForeignLanguageRuntimeFactoryDesc dump begin----");
                ASR_CORE_LOG_ERROR("{{\n{}\n}}", *up_plugin_desc);
                ASR_CORE_LOG_ERROR(
                    "----ForeignLanguageRuntimeFactoryDesc dump end----");

                Details::FailedPluginProxy failed_plugin{
                    plugin_name.Get(),
                    plugin_create_result};
                failed_plugin.AddPluginTo(map);
                result = ASR_S_FALSE;
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
                result = ASR_S_FALSE;
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

    return result;
}

AsrResult PluginManager::GetErrorMessage(
    const AsrGuid&       iid,
    AsrResult            error_code,
    IAsrReadOnlyString** pp_out_error_message)
{
    ASR_UTILS_CHECK_POINTER(pp_out_error_message)

    AsrResult result{ASR_E_UNDEFINED_RETURN_VALUE};

    AsrPtr<IAsrReadOnlyString> p_default_locale_name{};
    ::AsrGetDefaultLocale(p_default_locale_name.Put());

    error_lens_manager_
        .GetErrorMessage(iid, p_default_locale_name.Get(), error_code)
        .map(
            [&result, pp_out_error_message](const auto& p_error_message)
            {
                p_error_message->AddRef();
                *pp_out_error_message = p_error_message.Get();
                result = ASR_S_OK;
            })
        .or_else([&result](const auto ec) { result = ec; });

    return result;
}

AsrResult PluginManager::GetAllPluginInfo(
    IAsrPluginInfoVector** pp_out_plugin_info_vector)
{
    ASR_UTILS_CHECK_POINTER(pp_out_plugin_info_vector)

    const auto p_vector = MakeAsrPtr<AsrPluginInfoVectorImpl>();
    for (const auto& pair : name_plugin_map_)
    {
        const auto& plugin_desc = pair.second;
        p_vector->AddInfo(plugin_desc.GetInfo());
    }

    auto& p_out_plugin_info_vector = *pp_out_plugin_info_vector;
    p_out_plugin_info_vector = *p_vector.Get();
    p_out_plugin_info_vector->AddRef();

    return ASR_S_OK;
}

auto PluginManager::GetInterfaceStaticStorage(IAsrTypeInfo* p_type_info) const
    -> ASR::Utils::Expected<
        std::reference_wrapper<const InterfaceStaticStorage>>
{
    if (p_type_info == nullptr)
    {
        ASR_CORE_LOG_ERROR("Null pointer found. Please check your code.");
        return tl::make_unexpected(ASR_E_INVALID_POINTER);
    }

    AsrGuid guid;
    if (const auto gg_result = p_type_info->GetGuid(&guid); IsFailed(gg_result))
    {
        ASR_CORE_LOG_ERROR(
            "Get GUID failed. Pointer = {}",
            Utils::VoidP(p_type_info));
        return tl::make_unexpected(gg_result);
    }

    return Details::GetInterfaceStaticStorage(guid_storage_map_, guid);
}

auto PluginManager::GetInterfaceStaticStorage(IAsrSwigTypeInfo* p_type_info)
    const -> Asr::Utils::Expected<
        std::reference_wrapper<const InterfaceStaticStorage>>
{
    if (p_type_info == nullptr)
    {
        ASR_CORE_LOG_ERROR("Null pointer found. Please check your code.");
        return tl::make_unexpected(ASR_E_INVALID_POINTER);
    }

    const auto gg_result = p_type_info->GetGuid();
    if (IsFailed(gg_result.error_code))
    {
        ASR_CORE_LOG_ERROR(
            "Get GUID failed. Pointer = {}",
            Utils::VoidP(p_type_info));
        return tl::make_unexpected(gg_result.error_code);
    }

    return Details::GetInterfaceStaticStorage(
        guid_storage_map_,
        gg_result.value);
}

auto PluginManager::GetAllCaptureFactory() const noexcept
    -> const std::vector<AsrPtr<IAsrCaptureFactory>>&
{
    return capture_factory_vector_;
}

ASR_NS_ANONYMOUS_DETAILS_BEGIN

bool CheckIsFindInterfaceResultSucceed(
    const AsrResult error_code,
    const char*     variable_name)
{
    if (IsFailed(error_code) && error_code != ASR_E_NO_INTERFACE)
    {
        ASR_CORE_LOG_ERROR(
            "Error happend. Code = {}. Variable name = {}",
            error_code,
            variable_name);
        return false;
    }
    return true;
}

ASR_NS_ANONYMOUS_DETAILS_END

AsrResult PluginManager::FindInterface(const AsrGuid& iid, void** pp_out_object)
{
    AsrResult result{};

    result = task_manager_.FindInterface(
        iid,
        reinterpret_cast<IAsrTask**>(pp_out_object));

    if (Details::CheckIsFindInterfaceResultSucceed(result, "task_manager_"))
    {
        return result;
    }

    if (const auto factory_it = std::find_if(
            ASR_FULL_RANGE_OF(capture_factory_vector_),
            [&iid](const AsrPtr<IAsrCaptureFactory>& p_factory)
            {
                try
                {
                    const auto factory_iid =
                        Utils::GetGuidFrom(p_factory.Get());
                    return factory_iid == iid;
                }
                catch (const AsrException& ex)
                {
                    ASR_CORE_LOG_EXCEPTION(ex);
                    return false;
                }
            });
        factory_it != capture_factory_vector_.end())
    {
        *pp_out_object = factory_it->Get();
        factory_it->Get()->AddRef();
        return ASR_S_OK;
    }

    result = error_lens_manager_.FindInterface(
        iid,
        reinterpret_cast<IAsrErrorLens**>(pp_out_object));
    if (Details::CheckIsFindInterfaceResultSucceed(
            result,
            "error_lens_manager_"))
    {
        return result;
    }

    result = input_factory_manager_.FindInterface(
        iid,
        reinterpret_cast<IAsrInputFactory**>(pp_out_object));
    if (Details::CheckIsFindInterfaceResultSucceed(
            result,
            "input_factory_manager_"))
    {
        return result;
    }

    return ASR_E_NO_INTERFACE;
}

PluginManager::operator IAsrPluginManagerImpl*() noexcept
{
    return &cpp_projection_;
}

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

AsrResult InitializeIAsrPluginManager(
    IAsrReadOnlyGuidVector* p_ignore_plugins_guid,
    IAsrPluginManager**     pp_out_result)
{
    ASR_UTILS_CHECK_POINTER(pp_out_result)
    ASR_UTILS_CHECK_POINTER(p_ignore_plugins_guid)

    static size_t initialize_counter{0};
    ++initialize_counter;
    if (initialize_counter > 1)
    {
        ASR_CORE_LOG_ERROR(
            "The plugin should be loaded only once while the program is running");
    }

    return ASR_S_OK;
}

AsrResult CreateIAsrPluginManagerAndGetResult(
    IAsrReadOnlyGuidVector* p_ignore_plugins_guid,
    IAsrPluginManager**     pp_out_result)
{
    ASR_UTILS_CHECK_POINTER(p_ignore_plugins_guid)
    ASR_UTILS_CHECK_POINTER(pp_out_result)

    auto& plugin_manager = ASR::Core::ForeignInterfaceHost::g_plugin_manager;
    const auto result = plugin_manager.Refresh(p_ignore_plugins_guid);
    *pp_out_result = plugin_manager;
    return result;
}