#include "AdbCaptureFactoryImpl.h"
#include "PluginImpl.h"
#include <boost/url.hpp>
#include <das/DasApi.h>
#include <das/DasConfig.h>
#include <das/IDasBase.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/DasLogger.h>
#include <nlohmann/json.hpp>
#include <tl/expected.hpp>

DAS_NS_BEGIN

AdbCaptureFactoryImpl::AdbCaptureFactoryImpl() { AdbCaptureAddRef(); }

AdbCaptureFactoryImpl::~AdbCaptureFactoryImpl() { AdbCaptureRelease(); }

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto GetUrlFromJson(
    const nlohmann::json& config,
    const char** current_access_key) -> Das::Utils::Expected<boost::url_view>
{
    auto           on_exit = DAS::Utils::OnExit{[&current_access_key]()
                                      { *current_access_key = ""; }};
    constexpr auto url_string_literal = "url";

    *current_access_key = url_string_literal;
    const auto url_string = config[url_string_literal].get<std::string>();
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
    try
    {
        const auto config = nlohmann::json::parse(p_u8_environment_json);
        Details::GetUrlFromJson(config, &p_key_string);
    }
    catch (const nlohmann::json::exception& ex)
    {
        *pp_out_object = nullptr;
        DasLogErrorU8(DAS_UTILS_STRINGUTILS_DEFINE_U8STR("JSON Key: url"));
        DasLogErrorU8(
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR("----JSON dump begin----"));
        DasLogErrorU8(p_u8_environment_json);
        DasLogErrorU8(
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR("----JSON dump end----"));
        DasLogErrorU8(ex.what());
        result = DAS_E_INVALID_JSON;
    }
    return result;
}

DAS_NS_END