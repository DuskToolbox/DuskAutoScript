#ifndef DAS_IPLUGIN_H
#define DAS_IPLUGIN_H

#include <das/IDasBase.h>
#include <das/PluginInterface/IDasCapture.h>
#include <das/PluginInterface/IDasErrorLens.h>
#include <das/PluginInterface/IDasTask.h>
#include <memory>

typedef enum DasPluginFeature
{
    DAS_PLUGIN_FEATURE_CAPTURE_FACTORY = 0,
    DAS_PLUGIN_FEATURE_ERROR_LENS = 1,
    DAS_PLUGIN_FEATURE_TASK = 2,
    DAS_PLUGIN_FEATURE_INPUT_FACTORY = 4,
    DAS_PLUGIN_FEATURE_COMPONENT_FACTORY = 8,
    DAS_PLUGIN_FEATURE_FORCE_DWORD = 0x7FFFFFFF
} DasPluginFeature;

// {09EA2A40-6A10-4756-AB2B-41B2FD75AB36}
DAS_DEFINE_GUID(
    DAS_IID_PLUGIN,
    IDasPlugin,
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
SWIG_IGNORE(IDasPlugin)
/**
 * @brief plugin should define DasResult DasCoCreatePlugin(IDasPlugin**
 * pp_out_plugin);
 *
 */
DAS_INTERFACE IDasPlugin : public IDasBase
{
    DAS_METHOD EnumFeature(size_t index, DasPluginFeature * p_out_feature) = 0;
    DAS_METHOD CreateFeatureInterface(size_t index, void** pp_out_interface) =
        0;
    /**
     * @brief 插件检查是否还有已创建的接口实例存活，若有，返回 DAS_FALSE
     * ；否则返回 DAS_TRUE 。
     * @return DAS_FALSE 或 DAS_TRUE 。注意：非DAS_FALSE的值都会被认为是
     * DAS_TRUE。
     */
    DAS_METHOD CanUnloadNow() = 0;
};

SWIG_IGNORE(DASCOCREATEPLUGIN_NAME)
#define DASCOCREATEPLUGIN_NAME "DasCoCreatePlugin"
SWIG_IGNORE(DasCoCreatePluginFunction)
using DasCoCreatePluginFunction = DasResult(IDasPlugin** pp_out_plugin);

DAS_DEFINE_RET_TYPE(DasRetPluginFeature, DasPluginFeature);

// {3F11FBB2-B19F-4C3E-9502-B6D7F1FF9DAA}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_PLUGIN,
    IDasSwigPlugin,
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
DAS_SWIG_DIRECTOR_ATTRIBUTE(IDasSwigPlugin)
/**
 * @brief Plugin should define DasRetPlugin DasCoCreatePlugin()
 *
 */
DAS_INTERFACE IDasSwigPlugin : public IDasSwigBase
{
    virtual DasRetPluginFeature EnumFeature(size_t index) = 0;
    virtual DasRetSwigBase      CreateFeatureInterface(size_t index) = 0;
    virtual DasResult           CanUnloadPlugin() = 0;
};

DAS_DEFINE_RET_POINTER(DasRetPlugin, IDasSwigPlugin);

DAS_API DasResult
DasRegisterPluginObject(DasResult error_code, IDasSwigPlugin* p_swig_plugin);

#endif // DAS_IPLUGIN_H
