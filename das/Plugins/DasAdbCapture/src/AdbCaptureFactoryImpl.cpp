#include "AdbCaptureFactoryImpl.h"
#include "PluginImpl.h"
#include <boost/url.hpp>
#include <das/DasApi.h>
#include <das/DasConfig.h>
#include <das/IDasBase.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/DasLogger.h>
#include <tl/expected.hpp>

DAS_NS_BEGIN

AdbCaptureFactoryImpl::AdbCaptureFactoryImpl() { AdbCaptureAddRef(); }

AdbCaptureFactoryImpl::~AdbCaptureFactoryImpl() { AdbCaptureRelease(); }

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto GetUrlFromJson(
    const yyjson::writer::detail::value& config,
    const char** current_access_key) -> Das::Utils::Expected<boost::url_view>
{
    auto           on_exit = DAS::Utils::OnExit{[&current_access_key]()
                                                { *current_access_key = ""; }};
    constexpr auto url_string_literal = "url";

    *current_access_key = url_string_literal;
    auto obj = config.as_object();
    if (!obj)
    {
        const auto error_string =
            DAS::fmt::format("Config is not a JSON object");
        return tl::make_unexpected(DAS_E_INVALID_JSON);
    }
    auto url_val = (*obj)[std::string_view(url_string_literal)];
    auto url_str_opt = url_val.as_string();
    if (!url_str_opt)
    {
        const auto error_string =
            DAS::fmt::format("Missing 'url' field in config");
        return tl::make_unexpected(DAS_E_INVALID_JSON);
    }
    const auto url_string = std::string(*url_str_opt);
    const auto opt_url_view = boost::urls::parse_uri_reference(url_string);
    if (!opt_url_view.has_value()) [[unlikely]]
    {
        const auto error_string =
            DAS::fmt::format("Invalid URL: {}", url_string.data());
        return tl::make_unexpected(DAS_E_INVALID_URL);
    }
    return opt_url_view.value();
}

DAS_NS_ANONYMOUS_DETAILS_END

DasResult AdbCaptureFactoryImpl::CreateInstance(
    IDasReadOnlyString*            p_environment_json_config,
    IDasReadOnlyString*            p_plugin_config,
    PluginInterface::IDasCapture** pp_out_object)
{
    (void)p_plugin_config;
    const char* p_key_string = "";
    const char* p_u8_environment_json = nullptr;
    DasResult   result =
        p_environment_json_config->GetUtf8(&p_u8_environment_json);
    if (DAS::IsFailed(result))
    {
        *pp_out_object = nullptr;
        return result;
    }
    auto config_opt = Das::Utils::ParseYyjsonFromString(
        p_u8_environment_json ? std::string_view(p_u8_environment_json)
                              : std::string_view{});
    if (!config_opt)
    {
        *pp_out_object = nullptr;
        DasLogErrorU8(DAS_UTILS_STRINGUTILS_DEFINE_U8STR("JSON Key: url"));
        DasLogErrorU8(
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR("----JSON dump begin----"));
        DasLogErrorU8(p_u8_environment_json);
        DasLogErrorU8(
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR("----JSON dump end----"));
        DasLogErrorU8("Failed to parse JSON from environment config");
        return DAS_E_INVALID_JSON;
    }
    const auto config = std::move(*config_opt);
    auto       url_result = Details::GetUrlFromJson(config, &p_key_string);
    if (!url_result.has_value())
    {
        *pp_out_object = nullptr;
        DasLogErrorU8(DAS_UTILS_STRINGUTILS_DEFINE_U8STR("JSON Key: url"));
        DasLogErrorU8(
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR("----JSON dump begin----"));
        DasLogErrorU8(p_u8_environment_json);
        DasLogErrorU8(
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR("----JSON dump end----"));
        DasLogErrorU8("Failed to get URL from JSON config");
        return DAS_E_INVALID_JSON;
    }
    return DAS_S_OK;
}

DAS_NS_END