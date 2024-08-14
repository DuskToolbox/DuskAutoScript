#ifndef ASR_IPLUGIN_H
#define ASR_IPLUGIN_H

#include <AutoStarRail/IAsrBase.h>
#include <AutoStarRail/PluginInterface/IAsrCapture.h>
#include <AutoStarRail/PluginInterface/IAsrErrorLens.h>
#include <AutoStarRail/PluginInterface/IAsrTask.h>
#include <memory>

typedef enum AsrPluginFeature
{
    ASR_PLUGIN_FEATURE_CAPTURE_FACTORY = 0,
    ASR_PLUGIN_FEATURE_ERROR_LENS = 1,
    ASR_PLUGIN_FEATURE_TASK = 2,
    ASR_PLUGIN_FEATURE_INPUT_FACTORY = 4,
    ASR_PLUGIN_FEATURE_COMPONENT_FACTORY = 8,
    ASR_PLUGIN_FEATURE_FORCE_DWORD = 0x7FFFFFFF
} AsrPluginFeature;

// {09EA2A40-6A10-4756-AB2B-41B2FD75AB36}
ASR_DEFINE_GUID(
    ASR_IID_PLUGIN,
    IAsrPlugin,
    0x9ea2a40,
    0x6a10,
    0x4756,
    0xab,
    0x2b,
    0x41,
    0xb2,
    0xfd,
    0x75,
    0xab,
    0x36)
SWIG_IGNORE(IAsrPlugin)
/**
 * @brief plugin should define AsrResult AsrCoCreatePlugin(IAsrPlugin**
 * pp_out_plugin);
 *
 */
ASR_INTERFACE IAsrPlugin : public IAsrBase
{
    ASR_METHOD EnumFeature(size_t index, AsrPluginFeature * p_out_feature) = 0;
    ASR_METHOD CreateFeatureInterface(size_t index, void** pp_out_interface) =
        0;
    /**
     * @brief 插件检查是否还有已创建的接口实例存活，若有，返回 ASR_FALSE
     * ；否则返回 ASR_TRUE 。
     * @return ASR_FALSE 或 ASR_TRUE 。注意：非ASR_FALSE的值都会被认为是
     * ASR_TRUE。
     */
    ASR_METHOD CanUnloadNow() = 0;
};

SWIG_IGNORE(ASRCOCREATEPLUGIN_NAME)
#define ASRCOCREATEPLUGIN_NAME "AsrCoCreatePlugin"
SWIG_IGNORE(AsrCoCreatePluginFunction)
using AsrCoCreatePluginFunction = AsrResult(IAsrPlugin** pp_out_plugin);

ASR_DEFINE_RET_TYPE(AsrRetPluginFeature, AsrPluginFeature);

// {3F11FBB2-B19F-4C3E-9502-B6D7F1FF9DAA}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_PLUGIN,
    IAsrSwigPlugin,
    0x3f11fbb2,
    0xb19f,
    0x4c3e,
    0x95,
    0x2,
    0xb6,
    0xd7,
    0xf1,
    0xff,
    0x9d,
    0xaa);
ASR_SWIG_DIRECTOR_ATTRIBUTE(IAsrSwigPlugin)
/**
 * @brief Plugin should define AsrRetPlugin AsrCoCreatePlugin()
 *
 */
ASR_INTERFACE IAsrSwigPlugin : public IAsrSwigBase
{
    virtual AsrRetPluginFeature EnumFeature(size_t index) = 0;
    virtual AsrRetSwigBase      CreateFeatureInterface(size_t index) = 0;
    virtual AsrResult           CanUnloadPlugin() = 0;
};

ASR_DEFINE_RET_POINTER(AsrRetPlugin, IAsrSwigPlugin);

ASR_API AsrResult
AsrRegisterPluginObject(AsrResult error_code, IAsrSwigPlugin* p_swig_plugin);

#endif // ASR_IPLUGIN_H
