#include "AdbCaptureFactoryImpl.h"
#include "PluginImpl.h"
#include <AutoStarRail/AsrConfig.h>
#include <AutoStarRail/ExportInterface/AsrLogger.h>
#include <AutoStarRail/IAsrBase.h>
#include <AutoStarRail/Utils/QueryInterface.hpp>
#include <AutoStarRail/Utils/StringUtils.h>
#include <AutoStarRail/Utils/fmt.h>
#include <boost/url.hpp>
#include <nlohmann/json.hpp>
#include <tl/expected.hpp>

ASR_NS_BEGIN

AdbCaptureFactoryImpl::AdbCaptureFactoryImpl() { AdbCaptureAddRef(); }

AdbCaptureFactoryImpl::~AdbCaptureFactoryImpl() { AdbCaptureRelease(); }

int64_t AdbCaptureFactoryImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t AdbCaptureFactoryImpl::Release() { return ref_counter_.Release(this); }

AsrResult AdbCaptureFactoryImpl::QueryInterface(
    const AsrGuid& iid,
    void**         pp_object)
{
    return Utils::QueryInterface<IAsrCaptureFactory>(this, iid, pp_object);
}

ASR_NS_ANONYMOUS_DETAILS_BEGIN

auto GetUrlFromJson(
    const nlohmann::json& config,
    const char** current_access_key) -> Asr::Utils::Expected<boost::url_view>
{
    auto           on_exit = ASR::Utils::OnExit{[&current_access_key]()
                                      { *current_access_key = ""; }};
    constexpr auto url_string_literal = "url";

    *current_access_key = url_string_literal;
    const auto url_string = config[url_string_literal].get<std::string>();
    const auto opt_url_view = boost::urls::parse_uri_reference(url_string);
    if (!opt_url_view.has_value()) [[unlikely]]
    {
        const auto error_string =
            ASR::fmt::format("Invalid URL: {}", url_string.data());
        return tl::make_unexpected(ASR_E_INVALID_URL);
    }
    return opt_url_view.value();
}

ASR_NS_ANONYMOUS_DETAILS_END

AsrResult AdbCaptureFactoryImpl::CreateInstance(
    IAsrReadOnlyString* p_environment_json_config,
    IAsrReadOnlyString* p_plugin_config,
    IAsrCapture**       pp_out_object)
{
    const char* p_key_string = "";
    const char* p_json_config_string = nullptr;
    AsrResult   result = p_plugin_config->GetUtf8(&p_json_config_string);
    try
    {
        const auto config = nlohmann::json::parse(p_environment_json_config);
        Details::GetUrlFromJson(config, &p_key_string);
    }
    catch (const nlohmann::json::exception& ex)
    {
        *pp_out_object = nullptr;
        AsrLogErrorU8(ASR_UTILS_STRINGUTILS_DEFINE_U8STR("JSON Key: url"));
        AsrLogErrorU8(
            ASR_UTILS_STRINGUTILS_DEFINE_U8STR("----JSON dump begin----"));
        AsrLogErrorU8(p_json_config_string);
        AsrLogErrorU8(
            ASR_UTILS_STRINGUTILS_DEFINE_U8STR("----JSON dump end----"));
        AsrLogErrorU8(ex.what());
        result = ASR_E_INVALID_JSON;
    }
    return result;
}

ASR_IMPL AdbCaptureFactoryImpl::GetGuid(AsrGuid* p_out_guid)
{
    *p_out_guid = AsrIidOf<std::remove_pointer_t<decltype(this)>>();

    return ASR_S_OK;
}

AsrResult AdbCaptureFactoryImpl::GetRuntimeClassName(
    IAsrReadOnlyString** pp_out_class_name)
{
    ASR_UTILS_GET_RUNTIME_CLASS_NAME_IMPL(
        Asr::AdbCaptureFactoryImpl,
        pp_out_class_name);
}

ASR_NS_END