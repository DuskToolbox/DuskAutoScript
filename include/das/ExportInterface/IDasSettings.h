#ifndef DAS_SETTINGS_H
#define DAS_SETTINGS_H

#include <das/DasString.hpp>
#include <das/IDasBase.h>

DAS_INTERFACE IDasTypeInfo;
DAS_INTERFACE IDasSwigTypeInfo;

#ifndef SWIG
// {15D1BCD7-7922-447F-AD2C-17B838C1D53A}
DAS_DEFINE_GUID(
    DAS_IID_JSON_SETTING_ON_DELETED_HANDLER,
    IDasJsonSettingOnDeletedHandler,
    0x15d1bcd7,
    0x7922,
    0x447f,
    0xad,
    0x2c,
    0x17,
    0xb8,
    0x38,
    0xc1,
    0xd5,
    0x3a)
DAS_INTERFACE IDasJsonSettingOnDeletedHandler : public IDasBase
{
    DAS_METHOD OnDeleted() = 0;
};

// {56E5529D-C4EB-498D-BFAA-EFFEA20EB02A}
DAS_DEFINE_GUID(
    DAS_IID_JSON_SETTING,
    IDasJsonSetting,
    0x56e5529d,
    0xc4eb,
    0x498d,
    0xbf,
    0xaa,
    0xef,
    0xfe,
    0xa2,
    0xe,
    0xb0,
    0x2a);
DAS_INTERFACE IDasJsonSetting : public IDasBase
{
    /**
     * @brief 将json序列化为文本
     *
     * @param pp_out_string 输出的文本
     * @return DasResult DAS_S_OK表示成功，否则失败
     */
    DAS_METHOD ToString(IDasReadOnlyString * *pp_out_string) = 0;
    /**
     * @brief 将文本反序列化为json对象
     *
     * @param p_in_settings 输入的文本
     * @return DasResult DAS_S_OK表示成功，否则失败
     */
    DAS_METHOD FromString(IDasReadOnlyString * p_in_settings) = 0;
    /**
     * @brief 保存json对象到工作目录下的指定路径
     *
     * @return DasResult DAS_S_OK表示成功，否则失败
     */
    DAS_METHOD SaveToWorkingDirectory(IDasReadOnlyString * p_relative_path) = 0;
    /**
     * @brief 保存设置
     *
     * @return DasResult 保存设置文件
     */
    DAS_METHOD Save() = 0;
    /**
     * @brief 当设置项被删除时需要执行的回调
     */
    DAS_METHOD SetOnDeletedHandler(
        IDasJsonSettingOnDeletedHandler * p_handler) = 0;
};

typedef enum DasProfileProperty
{
    DAS_PROFILE_PROPERTY_PROFILE = 0,
    DAS_PROFILE_PROPERTY_SCHEDULER_STATE = 1,
    // -------------------------------------------------------------------------
    DAS_PROFILE_PROPERTY_NAME = 1001,
    DAS_PROFILE_PROPERTY_ID = 1002,
    DAS_PROFILE_PROPERTY_FORCE_DWORD = 0x7FFFFFFF
} DasProfileProperty;

// {774869F9-B453-4CA5-8512-B08E659383EA}
DAS_DEFINE_GUID(
    DAS_PROFILE,
    IDasProfile,
    0x774869f9,
    0xb453,
    0x4ca5,
    0x85,
    0x12,
    0xb0,
    0x8e,
    0x65,
    0x93,
    0x83,
    0xea);
DAS_INTERFACE IDasProfile : public IDasBase
{
    DAS_METHOD GetStringProperty(
        DasProfileProperty profile_property,
        IDasReadOnlyString * *pp_out_property) = 0;
    DAS_METHOD GetJsonSettingProperty(
        DasProfileProperty profile_property,
        IDasJsonSetting * *pp_out_json) = 0;
};

/**
 * @brief 将 ppp_out_profile
 * 置空，buffer_size将被忽略，返回值指示内部Profile数量
 */
DAS_GATEWAY_C_API DasResult
GetAllIDasProfile(size_t buffer_size, IDasProfile*** ppp_out_profile);

DAS_GATEWAY_C_API DasResult CreateIDasProfile(
    IDasReadOnlyString* p_profile_id,
    IDasReadOnlyString* p_profile_name,
    IDasReadOnlyString* p_profile_json);

DAS_GATEWAY_C_API DasResult DeleteIDasProfile(IDasReadOnlyString* p_profile_id);

DAS_GATEWAY_C_API DasResult
FindIDasProfile(IDasReadOnlyString* p_name, IDasProfile** pp_out_profile);

/**
 * @brief 加载UI在Core中寄存的json设置信息
 */
DAS_GATEWAY_C_API DasResult DasLoadExtraStringForUi(
    IDasReadOnlyString** pp_out_ui_extra_settings_json_string);

/**
 * @brief 保存UI在Core中寄存的json设置信息
 */
DAS_GATEWAY_C_API DasResult
DasSaveExtraStringForUi(IDasReadOnlyString* p_in_ui_extra_settings_json_string);

#endif // SWIG

#endif // DAS_SETTINGS_H
