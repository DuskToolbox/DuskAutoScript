#ifndef DAS_CORE_I18N_DASRESULTTRANSLATOR_H
#define DAS_CORE_I18N_DASRESULTTRANSLATOR_H

#include "i18n.hpp"

DAS_CORE_I18N_NS_BEGIN

/**
 * @brief 在项目使用全局locale以后，参数locale_name应当填入全局locale_name
 * @param locale_name
 * @param error_code
 * @param out_string
 * @return
 */
DasResult TranslateError(
    IDasReadOnlyString*  locale_name,
    DasResult            error_code,
    IDasReadOnlyString** pp_out_string);

extern const I18n<DasResult> g_translator_data;

DasResult GetExplanationWhenTranslateErrorFailed(
    const DasResult      unexplainable_error_code,
    const DasResult      error_code_that_failed_at_getting_error_explanation,
    IDasReadOnlyString** pp_out_string);

DAS_CORE_I18N_NS_END

#endif // DAS_CORE_I18N_DASRESULTTRANSLATOR_H