#ifndef ASR_SETTINGS_H
#define ASR_SETTINGS_H

#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/IAsrBase.h>

ASR_INTERFACE IAsrTypeInfo;
ASR_INTERFACE IAsrSwigTypeInfo;

typedef enum AsrType
{
    ASR_TYPE_INT = 0,
    ASR_TYPE_FLOAT = 1,
    ASR_TYPE_STRING = 2,
    ASR_TYPE_BOOL = 3,
    ASR_TYPE_FORCE_DWORD = 0x7FFFFFFF
} AsrType;

// {6180A529-2C54-4EA1-A6D0-892682662DD2}
ASR_DEFINE_GUID(
    ASR_IID_SETTINGS,
    IAsrSettings,
    0x6180a529,
    0x2c54,
    0x4ea1,
    0xa6,
    0xd0,
    0x89,
    0x26,
    0x82,
    0x66,
    0x2d,
    0xd2);
SWIG_IGNORE(IAsrSettings)
ASR_INTERFACE IAsrSettings : public IAsrBase
{
    ASR_METHOD GetString(
        IAsrReadOnlyString * key,
        IAsrReadOnlyString * *pp_out_string) = 0;
    ASR_METHOD GetBool(IAsrReadOnlyString * key, bool* p_out_bool) = 0;
    ASR_METHOD GetInt(IAsrReadOnlyString * key, int64_t * p_out_int) = 0;
    ASR_METHOD GetFloat(IAsrReadOnlyString * key, float* p_out_float) = 0;

    ASR_METHOD SetString(IAsrReadOnlyString * key, IAsrReadOnlyString * value) =
        0;
    ASR_METHOD SetBool(IAsrReadOnlyString * key, bool value) = 0;
    ASR_METHOD SetInt(IAsrReadOnlyString * key, int64_t value) = 0;
    ASR_METHOD SetFloat(IAsrReadOnlyString * key, float value) = 0;
};

// {0552065B-8FDF-46C7-82BA-703665E769EF}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_SETTINGS,
    IAsrSwigSettings,
    0x552065b,
    0x8fdf,
    0x46c7,
    0x82,
    0xba,
    0x70,
    0x36,
    0x65,
    0xe7,
    0x69,
    0xef)
ASR_INTERFACE IAsrSwigSettings : public IAsrSwigBase
{
    virtual AsrRetReadOnlyString GetString(AsrReadOnlyString key) = 0;
    virtual AsrRetBool           GetBool(AsrReadOnlyString key) = 0;
    virtual AsrRetInt            GetInt(AsrReadOnlyString key) = 0;
    virtual AsrRetFloat          GetFloat(AsrReadOnlyString key) = 0;

    virtual AsrResult SetString(
        AsrReadOnlyString key,
        AsrReadOnlyString value) = 0;
    virtual AsrResult SetBool(AsrReadOnlyString key, bool value) = 0;
    virtual AsrResult SetInt(AsrReadOnlyString key, int64_t value) = 0;
    virtual AsrResult SetFloat(AsrReadOnlyString key, float value) = 0;
};

#ifndef SWIG

// {56E5529D-C4EB-498D-BFAA-EFEFA20EB02A}
ASR_DEFINE_GUID(
    ASR_IID_SETTINGS_FOR_UI,
    IAsrSettingsForUi,
    0x56e5529d,
    0xc4eb,
    0x498d,
    0xbf,
    0xaa,
    0xef,
    0xef,
    0xa2,
    0xe,
    0xb0,
    0x2a);
ASR_INTERFACE IAsrSettingsForUi : public IAsrBase
{
    /**
     * @brief 将json序列化为文本
     *
     * @param pp_out_string 输出的文本
     * @return AsrResult ASR_S_OK表示成功，否则失败
     */
    ASR_METHOD ToString(IAsrReadOnlyString * *pp_out_string) = 0;
    /**
     * @brief 将文本反序列化为json对象
     *
     * @param p_in_settings 输入的文本
     * @return AsrResult ASR_S_OK表示成功，否则失败
     */
    ASR_METHOD FromString(IAsrReadOnlyString * p_in_settings) = 0;
    /**
     * @brief 保存json对象到工作目录下的指定路径
     *
     * @return AsrResult ASR_S_OK表示成功，否则失败
     */
    ASR_METHOD SaveToWorkingDirectory(IAsrReadOnlyString * p_relative_path) = 0;
    /**
     * @brief 保存设置
     * 
     * @return AsrResult 保存设置文件
     */
    ASR_METHOD Save() = 0;
};

/**
 * @brief 使用指定路径初始化Core设置，调用将从指定路径加载设置。
 *
 * @param p_settings_path 设置文件路径
 * @param pp_out_settings
 * @return AsrResult
 */
ASR_C_API AsrResult InitializeGlobalSettings(
    IAsrReadOnlyString* p_settings_path,
    IAsrSettingsForUi** pp_out_settings);

ASR_C_API AsrResult
GetPluginSettings(IAsrTypeInfo* p_plugin, IAsrSettings** pp_out_settings);

#endif // SWIG

#endif // ASR_SETTINGS_H
