#ifndef DAS_CORE_UTILS_IDASBASICERRORLENSIMPL_H
#define DAS_CORE_UTILS_IDASBASICERRORLENSIMPL_H

#include <das/Core/ForeignInterfaceHost/IDasGuidVectorImpl.h>
#include <das/Core/Utils/Config.h>
#include <das/ExportInterface/IDasBasicErrorLens.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/StringUtils.h>

DAS_CORE_UTILS_NS_BEGIN

class DasBasicErrorLensImpl;

class IDasBasicErrorLensImpl : public IDasBasicErrorLens
{
    DasBasicErrorLensImpl& impl_;

public:
    IDasBasicErrorLensImpl(DasBasicErrorLensImpl& impl);
    // IDasBase
    int64_t   AddRef() override;
    int64_t   Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;
    // IDasErrorLens
    DasResult GetSupportedIids(IDasReadOnlyGuidVector** pp_out_iids) override;
    DasResult GetErrorMessage(
        IDasReadOnlyString*  locale_name,
        DasResult            error_code,
        IDasReadOnlyString** pp_out_string) override;
    // IDasBasicErrorLens
    DasResult RegisterErrorMessage(
        IDasReadOnlyString* locale_name,
        DasResult           error_code,
        IDasReadOnlyString* p_explanation) override;
    DasResult GetWritableSupportedIids(IDasGuidVector** pp_out_iids) override;
};

class IDasSwigBasicErrorLensImpl : public IDasSwigBasicErrorLens
{
    DasBasicErrorLensImpl& impl_;

public:
    IDasSwigBasicErrorLensImpl(DasBasicErrorLensImpl& impl);
    int64_t                  AddRef() override;
    int64_t                  Release() override;
    DasRetSwigBase           QueryInterface(const DasGuid& iid) override;
    DasRetReadOnlyGuidVector GetSupportedIids() override;
    DasRetReadOnlyString     GetErrorMessage(
            const DasReadOnlyString locale_name,
            DasResult               error_code) override;
    DasResult RegisterErrorMessage(
        DasReadOnlyString locale_name,
        DasResult         error_code,
        DasReadOnlyString error_message) override;
    DasRetGuidVector GetWritableSupportedIids() override;
};

class DasBasicErrorLensImpl : DAS_UTILS_MULTIPLE_PROJECTION_GENERATORS(
                                  DasBasicErrorLensImpl,
                                  IDasBasicErrorLensImpl,
                                  IDasSwigBasicErrorLensImpl)
{
    using ErrorCodeMap =
        std::unordered_map<DasResult, DasPtr<IDasReadOnlyString>>;
    using LocaleErrorCodeMap = std::unordered_map<
        DasPtr<IDasReadOnlyString>,
        ErrorCodeMap,
        DasReadOnlyStringHash>;

    RefCounter<DasBasicErrorLensImpl>       ref_counter_{};
    LocaleErrorCodeMap                      map_{};
    ForeignInterfaceHost::DasGuidVectorImpl suppored_guid_vector_{};

public:
    int64_t AddRef();
    int64_t Release();

    DasResult GetSupportedIids(IDasReadOnlyGuidVector** pp_out_iids);
    DasResult GetErrorMessage(
        IDasReadOnlyString*  locale_name,
        DasResult            error_code,
        IDasReadOnlyString** out_string);

    DasResult RegisterErrorMessage(
        IDasReadOnlyString* locale_name,
        DasResult           error_code,
        IDasReadOnlyString* p_explanation);

    DasRetReadOnlyGuidVector GetSupportedIids();

    DasResult        GetWritableSupportedIids(IDasGuidVector** pp_out_iids);
    DasRetGuidVector GetWritableSupportedIids();
};

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_IDASBASICERRORLENSIMPL_H
