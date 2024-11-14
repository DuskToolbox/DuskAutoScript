#ifndef DAS_CORE_FOREIGNINTERFACEHOST_ERRORLENSMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_ERRORLENSMANAGER_H

#include <das/DasString.hpp>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/ExportInterface/IDasGuidVector.h>
#include <das/PluginInterface/IDasErrorLens.h>
#include <das/Utils/Expected.h>
#include <unordered_map>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class ErrorLensManager
{
private:
    std::unordered_map<DasGuid, DasPtr<IDasErrorLens>> map_{};

public:
    DasResult Register(
        IDasReadOnlyGuidVector* p_guid_vector,
        IDasErrorLens*          p_error_lens);
    DasResult Register(
        IDasSwigReadOnlyGuidVector* p_guid_vector,
        IDasSwigErrorLens*          p_error_lens);

    DasResult FindInterface(const DasGuid& iid, IDasErrorLens** pp_out_lens);

    auto GetErrorMessage(
        const DasGuid&      iid,
        IDasReadOnlyString* locale_name,
        DasResult           error_code) const
        -> DAS::Utils::Expected<DasPtr<IDasReadOnlyString>>;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_ERRORLENSMANAGER_H
