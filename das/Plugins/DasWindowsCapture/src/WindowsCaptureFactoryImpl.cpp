#include "WindowsCaptureFactoryImpl.h"
#include "PluginImpl.h"
#include <das/Core/DasWindowsCapture/WindowsCaptureImpl.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>
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

    const char* p_u8_plugin_config = nullptr;

    DasResult result = p_plugin_config->GetUtf8(&p_u8_plugin_config);

    if (FAILED(result))
    {
        DAS_LOG_ERROR("Failed to get plugin config UTF-8 string");
        return result;
    }

    auto config_opt = Das::Utils::ParseYyjsonFromString(
        p_u8_plugin_config ? std::string_view(p_u8_plugin_config)
                           : std::string_view{});
    if (!config_opt)
    {
        DAS_LOG_ERROR("Failed to parse plugin config JSON");
        *pp_out_object = nullptr;
        return DAS_E_INVALID_JSON;
    }
    auto config = std::move(*config_opt);

    auto obj = config.as_object();
    if (!obj)
    {
        DAS_LOG_ERROR("Plugin config is not a JSON object");
        *pp_out_object = nullptr;
        return DAS_E_INVALID_ARGUMENT;
    }

    auto cm_val = (*obj)[std::string_view("capture_mode")];
    auto cm_str = cm_val.as_string();
    if (!cm_str)
    {
        DAS_LOG_ERROR("Missing required 'capture_mode' in plugin config");
        *pp_out_object = nullptr;
        return DAS_E_INVALID_ARGUMENT;
    }

    std::string capture_mode = std::string(*cm_str);
    if (capture_mode != "windows_graphics_capture"
        && capture_mode != "gdi_bitblt")
    {
        DAS_LOG_ERROR("Invalid capture_mode");
        *pp_out_object = nullptr;
        return DAS_E_INVALID_ARGUMENT;
    }

    try
    {
        auto p_capture = new DAS::WindowsCapture{};
        if (!p_capture->ParseConfigAndSelectMode(config))
        {
            DAS_LOG_ERROR("Failed to parse config and select capture mode");
            delete p_capture;
            *pp_out_object = nullptr;
            return DAS_E_INVALID_ARGUMENT;
        }
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
