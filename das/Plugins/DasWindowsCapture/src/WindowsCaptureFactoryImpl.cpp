#include "WindowsCaptureFactoryImpl.h"
#include "PluginImpl.h"
#include <das/Core/DasWindowsCapture/WindowsCaptureImpl.h>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/DasLogger.h>
#include <das/_autogen/idl/abi/IDasReadOnlyString.h>
#include <nlohmann/json.hpp>
#include <stdexcept>

DAS_NS_BEGIN

WindowsCaptureFactoryImpl::WindowsCaptureFactoryImpl()
{
    WindowsCaptureAddRef();
}

WindowsCaptureFactoryImpl::~WindowsCaptureFactoryImpl()
{
    WindowsCaptureRelease();
}

DasResult WindowsCaptureFactoryImpl::CreateInstance(
    IDasReadOnlyString*            p_environment_json_config,
    IDasReadOnlyString*            p_plugin_config,
    PluginInterface::IDasCapture** pp_out_object)
{
    (void)p_environment_json_config;

    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_object);

    const char* p_config_string = "";
    const char* p_u8_plugin_config = nullptr;

    DasResult result = p_plugin_config->GetUtf8(&p_u8_plugin_config);

    if (FAILED(result))
    {
        DAS_LOG_ERROR("Failed to get plugin config UTF-8 string");
        return result;
    }

    nlohmann::json config;
    try
    {
        config = nlohmann::json::parse(p_u8_plugin_config);
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_LOG_ERROR("Failed to parse plugin config JSON: {}", ex.what());
        *pp_out_object = nullptr;
        return DAS_E_INVALID_JSON;
    }

    if (!config.contains("capture_mode"))
    {
        DAS_LOG_ERROR("Missing required 'capture_mode' in plugin config");
        *pp_out_object = nullptr;
        return DAS_E_INVALID_ARGUMENT;
    }

    std::string capture_mode = config["capture_mode"];
    if (capture_mode != "windows_graphics_capture"
        && capture_mode != "gdi_bitblt")
    {
        DAS_LOG_ERROR(
            "Invalid capture_mode: {}. Expected 'windows_graphics_capture' or 'gdi_bitblt'",
            capture_mode);
        *pp_out_object = nullptr;
        return DAS_E_INVALID_ARGUMENT;
    }

    try
    {
        const auto p_capture = new DAS::WindowsCapture{config};
        *pp_out_object = p_capture;
        p_capture->AddRef();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_LOG_ERROR("Out of memory creating WindowsCapture instance");
        *pp_out_object = nullptr;
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        DAS_LOG_ERROR("Unknown exception creating WindowsCapture instance");
        *pp_out_object = nullptr;
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
}

DAS_NS_END
