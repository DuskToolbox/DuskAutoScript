#ifndef ASR_SETTINGS_H
#define ASR_SETTINGS_H

#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/IAsrBase.h>

ASR_INTERFACE IAsrTypeInfo;
ASR_INTERFACE IAsrSwigTypeInfo;

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
    IAsrReadOnlyString* p_in_ui_extra_settings_json_string);

#endif // SWIG

#endif // ASR_SETTINGS_H
