#include "AdbTouchFactoryImpl.h"
#include "AdbTouch.h"
#include <das/_autogen/idl/abi/DasLogger.h>
#include <boost/url.hpp>
#include <das/DasApi.h>
#include <das/Utils/StringUtils.h>
#include <nlohmann/json.hpp>


DAS_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_UNUSED_FUNCTION

struct AdbConnectionDesc
{
    std::string type{};
    std::string url{};
    std::string adbPath{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AdbConnectionDesc, type, url, adbPath);
};

DAS_DISABLE_WARNING_END

// {6B36D95E-96D1-4642-8426-3EA0514662E6}
const DasGuid DAS_IID_ADB_INPUT_FACTORY = {
    0x6b36d95e,
    0x96d1,
    0x4642,
    {0x84, 0x26, 0x3e, 0xa0, 0x51, 0x46, 0x62, 0xe6}};

DAS_NS_ANONYMOUS_DETAILS_END

DasResult AdbTouchFactory::QueryInterface(const DasGuid& iid, void** pp_object)
{
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_object);

    // 检查IID_IDasInputFactory
    if (iid == DasIidOf<PluginInterface::IDasInputFactory>())
    {
        *pp_object = static_cast<PluginInterface::IDasInputFactory*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // 检查IID_IDasTypeInfo
    if (iid == DAS_IID_TYPE_INFO)
    {
        *pp_object = static_cast<IDasTypeInfo*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // 检查IID_IDasBase
    if (iid == DAS_IID_BASE)
    {
        *pp_object = static_cast<IDasBase*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    *pp_object = nullptr;
    return DAS_E_NO_INTERFACE;
}

DasResult AdbTouchFactory::GetRuntimeClassName(IDasReadOnlyString** pp_out_name)
{
    const auto u8_name =
        DAS_UTILS_STRINGUTILS_DEFINE_U8STR("Das::AdbInputFactory");
    return ::CreateIDasReadOnlyStringFromUtf8(u8_name, pp_out_name);
}

DasResult AdbTouchFactory::GetGuid(DasGuid* p_out_guid)
{
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_guid);

    *p_out_guid = Details::DAS_IID_ADB_INPUT_FACTORY;

    return DAS_S_OK;
}

DasResult AdbTouchFactory::CreateInstance(
    IDasReadOnlyString* p_json_config,
    IDasInput**         pp_out_input)
{
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_input);

    const char* p_u8_json_config{nullptr};
    if (const auto get_u8_string_result =
            p_json_config->GetUtf8(&p_u8_json_config);
        IsFailed(get_u8_string_result))
    {
        return get_u8_string_result;
    }

    Details::AdbConnectionDesc connection_desc{};

    try
    {
        const auto config = nlohmann::json::parse(p_u8_json_config);

        config.at("connection").get_to(connection_desc);

        const auto adb_url = boost::url{connection_desc.url};
        if (adb_url.scheme() != std::string_view{"adb"})
        {
            const auto error_message = fmt::format(
                "Unexpected adb url. Input = {} .",
                connection_desc.url);
            DAS_LOG_ERROR(error_message.c_str());
            return DAS_E_INVALID_URL;
        }
        const auto p_result =
            new AdbTouch{connection_desc.adbPath, adb_url.authority().buffer()};
        *pp_out_input = p_result;
        p_result->AddRef();
        return DAS_S_OK;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_LOG_ERROR(
            "Can not parse json config. Error message and json dump is below:");
        DAS_LOG_ERROR(ex.what());
        DAS_LOG_ERROR(p_u8_json_config);
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_LOG_ERROR(ex.what());
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (const boost::system::error_code& ex)
    {
        const auto error_message = fmt::format(
            "Parsing url failed. Error message = {}. Input = {}",
            ex.what(),
            connection_desc.url);
        DAS_LOG_ERROR(error_message.c_str());
        return DAS_E_INVALID_URL;
    }
}

DAS_NS_END
