#ifndef DAS_SETTINGS_H
#define DAS_SETTINGS_H

#include <das/DasString.hpp>
#include <das/IDasBase.h>

DAS_INTERFACE IDasTypeInfo;
DAS_INTERFACE IDasSwigTypeInfo;

#ifndef SWIG

// {56E5529D-C4EB-498D-BFAA-EFFEA20EB02A}
DAS_DEFINE_GUID(
    DAS_IID_SETTINGS_FOR_UI,
    IDasSettingsForUi,
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
DAS_INTERFACE IDasSettingsForUi : public IDasBase
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
};

/**
 * @brief 读取加载的Core设置以供UI使用
 *
 * @param pp_out_settings 由UI使用的设置
 * @return DasResult DAS_S_OK 成功；若从未执行过DasLoadGlobalSettings，则失败。
 */
DAS_C_API DasResult DasGetGlobalSettings(IDasSettingsForUi** pp_out_settings);

/**
 * @brief 加载UI在Core中寄存的json设置信息
 */
DAS_C_API DasResult DasLoadExtraStringForUi(
    IDasReadOnlyString** pp_out_ui_extra_settings_json_string);

/**
 * @brief 保存UI在Core中寄存的json设置信息
 */
DAS_C_API DasResult DasSaveExtraStringForUi(
    IDasReadOnlyString* p_in_ui_extra_settings_json_string);

#endif // SWIG

#endif // DAS_SETTINGS_H
