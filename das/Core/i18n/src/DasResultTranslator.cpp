#include <das/Core/i18n/DasResultTranslator.h>
#include <das/Core/i18n/GlobalLocale.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Utils/fmt.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/Expected.h>
#include <das/PluginInterface/IDasErrorLens.h>

DAS_CORE_I18N_NS_BEGIN

DasResult TranslateError(
    IDasReadOnlyString*  locale_name,
    DasResult            error_code,
    IDasReadOnlyString** pp_out_string)
{
    if (pp_out_string == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    const char*    p_locale_name{};
    const char8_t* p_u8_locale_name{};

    auto result = locale_name->GetUtf8(&p_locale_name);
    if (!DAS::IsOk(result))
    {
        DAS_CORE_LOG_WARN(
            "Failed to get local name from string pointer. DasCore will use \"en\" instead.");
        p_u8_locale_name = u8"en";
    }
    else
    {
        p_u8_locale_name = reinterpret_cast<const char8_t*>(p_locale_name);
    }

    result = g_translator_data.GetErrorMessage(
        p_u8_locale_name,
        error_code,
        pp_out_string);

    if (!DAS::IsOk(result))
    {
        const auto error_string = DAS::fmt::format(
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR(
                "Error happened when getting error explanation. Code = {} ."),
            result);
        DAS_CORE_LOG_ERROR(error_string);
        DasPtr<IDasString> p_error_string;
        ::CreateIDasStringFromUtf8(error_string.c_str(), p_error_string.Put());
        p_error_string->AddRef();
        *pp_out_string = p_error_string.Get();
        return result;
    }

    return result;
}

I18n<DasResult> MakeDasResultTranslatorData()
{
    TranslateResources<DasResult, DasReadOnlyStringWrapper> translate_resource{
        {u8"en",
         {{DAS_S_OK, u8"Success"},
          {DAS_E_NO_INTERFACE, u8"No interface"},
          {DAS_E_UNDEFINED_RETURN_VALUE, u8"Return value not defined"},
          {DAS_E_INVALID_STRING, u8"Invalid string"},
          {DAS_E_INVALID_STRING_SIZE, u8"Invalid string size"},
          {DAS_E_NO_IMPLEMENTATION, u8"No implementation"},
          {DAS_E_UNSUPPORTED_SYSTEM, u8"Unsupported system"},
          {DAS_E_INVALID_JSON, u8"Invalid JSON"}}},
        {
            u8"zh-cn",
            {{{DAS_S_OK, u8"成功"},
              {DAS_E_NO_INTERFACE, u8"接口未找到"},
              {DAS_E_UNDEFINED_RETURN_VALUE, u8"接口没有处理返回值"},
              {DAS_E_INVALID_STRING, u8"非法字符串"},
              {DAS_E_INVALID_STRING_SIZE, u8"非法字符串长度"},
              {DAS_E_NO_IMPLEMENTATION, u8"未实现"},
              {DAS_E_UNSUPPORTED_SYSTEM, u8"不支持的操作系统"},
              {DAS_E_INVALID_JSON, u8"非法的JSON数据"}}},
        }};
    decltype(g_translator_data) result{std::move(translate_resource)};
    return result;
}

DAS_DEFINE_VARIABLE(g_translator_data) = MakeDasResultTranslatorData();

DAS_NS_ANONYMOUS_DETAILS_BEGIN

struct CharComparator
{
    bool operator()(const char* const p_lhs, const char* const p_rhs)
        const noexcept
    {
        return (std::strcmp(p_lhs, p_rhs) < 0);
    }
};

const std::map<const char*, const char*, CharComparator>
    g_translate_error_failed_explanation{
        {DAS_UTILS_STRINGUTILS_DEFINE_U8STR("zh-cn"),
         DAS_UTILS_STRINGUTILS_DEFINE_U8STR(
             "无法检索到错误码（值为{}）的解释。错误码：{}。")},
        {DAS_UTILS_STRINGUTILS_DEFINE_U8STR("us"),
         DAS_UTILS_STRINGUTILS_DEFINE_U8STR(
             "Can not find error code (value = {}) explanation. Error code: {}.")}};

auto FormatUnexplainableError(
    const DasResult   error_code_that_failed_at_getting_error_explanation,
    const DasResult   error_code,
    const char* const p_explanation_template,
    DasPtr<IDasReadOnlyString>& in_out_error_string,
    IDasReadOnlyString**        pp_out_string) -> DAS::Utils::Expected<void>
{
    const auto explanation = DAS::fmt::vformat(
        p_explanation_template,
        DAS::fmt::make_format_args(
            error_code_that_failed_at_getting_error_explanation,
            error_code));
    const auto create_string_result = ::CreateIDasReadOnlyStringFromUtf8(
        explanation.c_str(),
        in_out_error_string.Put());
    if (DAS::IsOk(create_string_result)) [[likely]]
    {
        in_out_error_string->AddRef();
        *pp_out_string = in_out_error_string.Get();
        return {};
    }
    else [[unlikely]]
    {
        DAS_CORE_LOG_ERROR(
            "Failed to create IDasReadOnlyString. Error code: {}.",
            create_string_result);
        *pp_out_string = nullptr;
        return tl::make_unexpected(create_string_result);
    }
}

DAS_NS_ANONYMOUS_DETAILS_END

/**
 *
 * @param unexplainable_error_code 无法解释的错误码
 * @param error_code_that_failed_at_getting_error_explanation
 * 请求解释时失败的错误码
 * @param pp_out_string 输出的文本
 */
DasResult GetExplanationWhenTranslateErrorFailed(
    const DasResult      unexplainable_error_code,
    const DasResult      error_code_that_failed_at_getting_error_explanation,
    IDasReadOnlyString** pp_out_string)
{
    DasResult                       result{DAS_E_UNDEFINED_RETURN_VALUE};
    DAS::DasPtr<IDasReadOnlyString> p_error_string{};
    auto                            default_locale = ::DasGetDefaultLocale();
    const auto* const p_locale_name = default_locale.value.GetUtf8();

    if (const auto it =
            Details::g_translate_error_failed_explanation.find(p_locale_name);
        it != Details::g_translate_error_failed_explanation.end())
    {
        const auto* const p_explanation_template = it->second;
        Details::FormatUnexplainableError(
            error_code_that_failed_at_getting_error_explanation,
            unexplainable_error_code,
            p_explanation_template,
            p_error_string,
            pp_out_string)
            .or_else([&result](const DasResult ec) { result = ec; });
    }
    else
    {
        const auto  fallback_locale = DAS::Core::i18n::GetFallbackLocale();
        const char* u8_fallback_locale{};
        fallback_locale->GetUtf8(&u8_fallback_locale);
        // 这里不应该抛出异常
        const auto* const p_explanation_template =
            Details::g_translate_error_failed_explanation.at(
                u8_fallback_locale);
        Details::FormatUnexplainableError(
            error_code_that_failed_at_getting_error_explanation,
            unexplainable_error_code,
            p_explanation_template,
            p_error_string,
            pp_out_string)
            .or_else([&result](const DasResult ec) { result = ec; });
    }
    return result;
}

DAS_CORE_I18N_NS_END