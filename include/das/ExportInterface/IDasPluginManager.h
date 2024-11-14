#ifndef DAS_PLUGINMANAGER_H
#define DAS_PLUGINMANAGER_H

/**
 * @file IDasPluginManager.h
 * @brief The exported interface in this file should only be used by GUI
 * programs.
 * @version 0.1
 * @date 2023-07-18
 *
 * @copyright Copyright (c) 2023
 *
 */

#include <das/DasExport.h>
#include <das/DasString.hpp>
#include <das/ExportInterface/IDasCaptureManager.h>
#include <das/ExportInterface/IDasGuidVector.h>
#include <das/IDasBase.h>
#include <das/PluginInterface/IDasComponent.h>
#include <cstddef>

// {8179F162-5E1A-4248-AC67-758D2AFF18A7}
DAS_DEFINE_GUID(
    DAS_IID_PLUGIN_INFO,
    IDasPluginInfo,
    0x8179f162,
    0x5e1a,
    0x4248,
    0xac,
    0x67,
    0x75,
    0x8d,
    0x2a,
    0xff,
    0x18,
    0xa7);
SWIG_IGNORE(IDasPluginInfo)
DAS_INTERFACE IDasPluginInfo : public IDasBase
{
    DAS_METHOD GetName(IDasReadOnlyString * *pp_out_name) = 0;
    DAS_METHOD GetDescription(IDasReadOnlyString * *pp_out_description) = 0;
    DAS_METHOD GetAuthor(IDasReadOnlyString * *pp_out_author) = 0;
    DAS_METHOD GetVersion(IDasReadOnlyString * *pp_out_version) = 0;
    DAS_METHOD GetSupportedSystem(
        IDasReadOnlyString * *pp_out_supported_system) = 0;
    DAS_METHOD GetPluginIid(DasGuid * p_out_guid) = 0;
    DAS_METHOD GetPluginSettingsDescriptor(IDasReadOnlyString** pp_out_string) = 0;
};

// {138DF2D2-A9E9-4A73-9B4F-AA6C754601CC}
DAS_DEFINE_GUID(
    DAS_IID_PLUGIN_INFO_VECTOR,
    IDasPluginInfoVector,
    0x138df2d2,
    0xa9e9,
    0x4a73,
    0x9b,
    0x4f,
    0xaa,
    0x6c,
    0x75,
    0x46,
    0x1,
    0xcc);
SWIG_IGNORE(IDasPluginInfoVector)
DAS_INTERFACE IDasPluginInfoVector : public IDasBase
{
    DAS_METHOD Size(size_t * p_out_size) = 0;
    DAS_METHOD At(size_t index, IDasPluginInfo * *pp_out_info) = 0;
};

// {CBEBF351-F4EE-4981-A0AB-69EC5562F08D}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_PLGUIN_INFO,
    IDasSwigPluginInfo,
    0xcbebf351,
    0xf4ee,
    0x4981,
    0xa0,
    0xab,
    0x69,
    0xec,
    0x55,
    0x62,
    0xf0,
    0x8d);
DAS_SWIG_EXPORT_ATTRIBUTE(IDasSwigPluginInfo)
DAS_INTERFACE IDasSwigPluginInfo : public IDasSwigBase
{
    virtual DasRetReadOnlyString GetName() = 0;
    virtual DasRetReadOnlyString GetDescription() = 0;
    virtual DasRetReadOnlyString GetAuthor() = 0;
    virtual DasRetReadOnlyString GetVersion() = 0;
    virtual DasRetReadOnlyString GetSupportedSystem() = 0;
    virtual DasRetGuid           GetPluginIid() = 0;
};

DAS_DEFINE_RET_POINTER(DasRetPluginInfo, IDasSwigPluginInfo);

// {30CCAE61-3884-43F4-AE78-976410156370}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_PLUGIN_INFO_VECTOR,
    IDasSwigPluginInfoVector,
    0x30ccae61,
    0x3884,
    0x43f4,
    0xae,
    0x78,
    0x97,
    0x64,
    0x10,
    0x15,
    0x63,
    0x70);
DAS_SWIG_EXPORT_ATTRIBUTE(IDasSwigPluginInfoVector)
DAS_INTERFACE IDasSwigPluginInfoVector : public IDasSwigBase
{
    virtual DasRetUInt       Size() = 0;
    virtual DasRetPluginInfo At(size_t index) = 0;
};

// {C665F0C7-F766-4151-802A-533BDCE72D90}
DAS_DEFINE_GUID(
    DAS_IID_PLUGIN_MANAGER_FOR_UI,
    IDasPluginManagerForUi,
    0xc665f0c7,
    0xf766,
    0x4151,
    0x80,
    0x2a,
    0x53,
    0x3b,
    0xdc,
    0xe7,
    0x2d,
    0x90);
SWIG_IGNORE(IDasPluginManagerForUi)
DAS_INTERFACE IDasPluginManagerForUi : public IDasBase
{
    DAS_METHOD GetAllPluginInfo(
        IDasPluginInfoVector * *pp_out_plugin_info_vector) = 0;
    DAS_METHOD FindInterface(const DasGuid& iid, void** pp_object) = 0;
    DAS_METHOD GetPluginSettingsJson(
        const DasGuid&       plugin_guid,
        IDasReadOnlyString** pp_out_json) = 0;
    DAS_METHOD SetPluginSettingsJson(
        const DasGuid&      plugin_guid,
        IDasReadOnlyString* p_json) = 0;
    DAS_METHOD ResetPluginSettings(const DasGuid& plugin_guid) = 0;
};

// {B2678FF8-720C-48E6-AC00-77D43D08F580}
DAS_DEFINE_GUID(
    DAS_IID_PLUGIN_MANAGER,
    IDasPluginManager,
    0xb2678ff8,
    0x720c,
    0x48e6,
    0xac,
    0x0,
    0x77,
    0xd4,
    0x3d,
    0x8,
    0xf5,
    0x80);
SWIG_IGNORE(IDasPluginManager)
DAS_INTERFACE IDasPluginManager : public IDasBase
{
    DAS_METHOD CreateComponent(
        const DasGuid&  iid,
        IDasComponent** pp_out_component) = 0;
    DAS_METHOD CreateCaptureManager(
        IDasReadOnlyString * p_environment_config,
        IDasCaptureManager * *pp_out_capture_manager) = 0;
};

// {064CBDE3-C1BC-40A7-9B8E-037F91727D46}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_PLUGIN_MANAGER,
    IDasSwigPluginManager,
    0x64cbde3,
    0xc1bc,
    0x40a7,
    0x9b,
    0x8e,
    0x3,
    0x7f,
    0x91,
    0x72,
    0x7d,
    0x46);
DAS_SWIG_EXPORT_ATTRIBUTE(IDasSwigPluginManager)
DAS_INTERFACE IDasSwigPluginManager : public IDasSwigBase
{
    virtual DasRetComponent      CreateComponent(const DasGuid& iid) = 0;
    virtual DasRetCaptureManager CreateCaptureManager(
        DasReadOnlyString environment_config) = 0;
};

DAS_DEFINE_RET_POINTER(DasRetPluginManager, IDasSwigPluginManager);

// {550B0110-23D2-4755-A822-AB4CB2B6BF06}
DAS_DEFINE_GUID(
    DAS_IID_INITIALIZE_IDAS_PLUGIN_MANAGER_CALLBACK,
    IDasInitializeIDasPluginManagerCallback,
    0x550b0110,
    0x23d2,
    0x4755,
    0xa8,
    0x22,
    0xab,
    0x4c,
    0xb2,
    0xb6,
    0xbf,
    0x6);
SWIG_IGNORE(IDasInitializeIDasPluginManagerCallback)
DAS_INTERFACE IDasInitializeIDasPluginManagerCallback : public IDasBase
{
    DAS_METHOD OnFinished(DasResult initialize_result) = 0;
};

// {32146CA1-C81F-4EBC-BE84-12F1F25114EE}
DAS_DEFINE_GUID(
    DAS_IID_INITIALIZE_IDAS_PLUGIN_MANAGER_WAITER,
    IDasInitializeIDasPluginManagerWaiter,
    0x32146ca1,
    0xc81f,
    0x4ebc,
    0xbe,
    0x84,
    0x12,
    0xf1,
    0xf2,
    0x51,
    0x14,
    0xee);
SWIG_IGNORE(IDasInitializeIDasPluginManagerWaiter)
DAS_INTERFACE IDasInitializeIDasPluginManagerWaiter : public IDasBase
{
    DAS_METHOD Wait() = 0;
};

/**
 * @brief
 * 异步地初始化插件管理器单例，提供需要被禁用的插件的GUID，并内部记录调用线程的id
 * 下列API调用顺序为：DasHttp或其它host调用 InitializeIDasPluginManager ->
 * CreateIDasPluginManagerAndGetResult
 * 然后内部插件或DasHttp或其它host才能调用 GetExistingIDasPluginManager
 *
 * @return DasResult S_OK表示成功初始化，S_FALSE表示已经初始化
 */
SWIG_IGNORE(InitializeIDasPluginManager)
DAS_C_API DasResult InitializeIDasPluginManager(
    IDasReadOnlyGuidVector*                  p_ignore_plugins_guid,
    IDasInitializeIDasPluginManagerCallback* p_on_finished,
    IDasInitializeIDasPluginManagerWaiter**  pp_out_waiter);

/**
 * @brief Call this function to load all plugin.
 *
 * @param pp_out_result
 * @return DasResult
 */
SWIG_IGNORE(CreateIDasPluginManagerAndGetResult)
DAS_C_API DasResult CreateIDasPluginManagerAndGetResult(
    IDasReadOnlyGuidVector* p_ignore_plugins_guid,
    IDasPluginManager**     pp_out_result);

/**
 * @brief 获取现有的插件管理器单例
 *
 * @param pp_out_result
 * @return DasResult S_OK 返回现有的插件管理器；
 * DAS_E_OBJECT_NOT_INIT 表示未初始化，调用者不应当遇到这一错误
 */
SWIG_IGNORE(CreateIDasPluginManagerAndGetResult)
DAS_C_API DasResult
GetExistingIDasPluginManager(IDasPluginManager** pp_out_result);

DAS_API DasRetPluginManager GetExistingIDasPluginManager();

#endif // DAS_PLUGINMANAGER_H
