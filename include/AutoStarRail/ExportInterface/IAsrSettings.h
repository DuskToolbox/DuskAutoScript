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
    ASR_TYPE_BOOL = 4,
    ASR_TYPE_OBJECT = 8,
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

// {56E5529D-C4EB-498D-BFAA-EFFEA20EB02A}
ASR_DEFINE_GUID(
    ASR_IID_SETTINGS_FOR_UI,
    IAsrSettingsForUi,
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
 * @brief 使用指定路径加载Core设置，调用在整个程序生命周期中只能执行一次。
 *
 * @param p_settings_path 设置文件路径
 * @param pp_out_settings
 * @return AsrResult
 */
ASR_C_API AsrResult AsrLoadGlobalSettings(IAsrReadOnlyString* p_settings_path);

/**
 * @brief 读取加载的Core设置以供UI使用
 *
 * @param pp_out_settings 由UI使用的设置
 * @return AsrResult ASR_S_OK 成功；若从未执行过AsrLoadGlobalSettings，则失败。
 */
ASR_C_API AsrResult AsrGetGlobalSettings(IAsrSettingsForUi** pp_out_settings);

/**
 * @brief 加载UI在Core中寄存的json设置信息
 */
ASR_C_API AsrResult AsrLoadExtraStringForUi(
    IAsrReadOnlyString** pp_out_ui_extra_settings_json_string);

/**
 * @brief 保存UI在Core中寄存的json设置信息
 */
ASR_C_API AsrResult AsrSaveExtraStringForUi(
    IAsrReadOnlyString* p_out_ui_extra_settings_json_string);

#endif // SWIG

#endif // ASR_SETTINGS_H
