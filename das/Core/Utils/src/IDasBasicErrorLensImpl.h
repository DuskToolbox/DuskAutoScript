#ifndef DAS_CORE_UTILS_IDASBASICERRORLENSIMPL_H
#define DAS_CORE_UTILS_IDASBASICERRORLENSIMPL_H

#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasBasicErrorLens.Implements.hpp>
#include <das/Core/ForeignInterfaceHost/IDasGuidVectorImpl.h>
#include <das/Core/Utils/Config.h>
#include <das/Utils/StringUtils.h>

DAS_CORE_UTILS_NS_BEGIN

using namespace DAS::ExportInterface;
class DasBasicErrorLensImpl final
    : public PluginInterface::DasBasicErrorLensImplBase<DasBasicErrorLensImpl>
{
    using ErrorCodeMap =
        std::unordered_map<DasResult, DasPtr<IDasReadOnlyString>>;
    using LocaleErrorCodeMap = std::unordered_map<
        DasPtr<IDasReadOnlyString>,
        ErrorCodeMap,
        DasReadOnlyStringHash>;

    LocaleErrorCodeMap                      map_{};
    ForeignInterfaceHost::DasGuidVectorImpl suppored_guid_vector_{};

public:
    DasResult GetSupportedIids(IDasReadOnlyGuidVector** pp_out_iids) override;
    DasResult GetErrorMessage(
        IDasReadOnlyString*  locale_name,
        DasResult            error_code,
        IDasReadOnlyString** out_string) override;
    DasResult RegisterErrorMessage(
        IDasReadOnlyString* locale_name,
        DasResult           error_code,
        IDasReadOnlyString* p_explanation) override;
    DasResult GetWritableSupportedIids(IDasGuidVector** pp_out_iids) override;
};

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_IDASBASICERRORLENSIMPL_H
