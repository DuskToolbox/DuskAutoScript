#ifndef DAS_PLUGINS_DASADBCAPTURE_ERRORLENSIMPL_H
#define DAS_PLUGINS_DASADBCAPTURE_ERRORLENSIMPL_H

#include <das/PluginInterface/IDasErrorLens.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <string>
#include <unordered_map>

DAS_NS_BEGIN

constexpr DasResult CAPTURE_DATA_TOO_LESS = -1;
constexpr DasResult UNSUPPORTED_COLOR_FORMAT = -2;

class AdbCaptureErrorLens final : public IDasErrorLens
{
    using ErrorCodeMap =
        std::unordered_map<DasResult, DasPtr<IDasReadOnlyString>>;
    using LocaleErrorCodeMap = std::unordered_map<
        DasPtr<IDasReadOnlyString>,
        ErrorCodeMap,
        Utils::DasReadOnlyStringHash>;
    LocaleErrorCodeMap                          map_;
    DAS::Utils::RefCounter<AdbCaptureErrorLens> ref_counter_;
    std::vector<DasGuid>                        iids_;

    static DasPtr<IDasReadOnlyString> p_default_locale_name;
    static std::string (*error_code_not_found_explanation_generator)(
        DasResult,
        IDasReadOnlyString**);

public:
    AdbCaptureErrorLens();
    ~AdbCaptureErrorLens();
    // IDasBase
    int64_t   AddRef() override;
    int64_t   Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override;
    // IDasErrorLens
    DasResult GetSupportedIids(IDasReadOnlyGuidVector** pp_out_iids) override;
    DasResult GetErrorMessage(
        IDasReadOnlyString*  locale_name,
        DasResult            error_code,
        IDasReadOnlyString** out_string) override;
    DasResult RegisterErrorCode(
        const DasResult            error_code,
        DasPtr<IDasReadOnlyString> locale_name,
        DasPtr<IDasReadOnlyString> p_explanation);
    DasResult AddSupportedIid(const DasGuid& iid);
};

DAS_NS_END

#endif // DAS_PLUGINS_DASADBCAPTURE_ERRORLENSIMPL_H