#ifndef DAS_CORE_FOREIGNINTERFACEHOST_ERRORLENSMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_ERRORLENSMANAGER_H

#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/DasString.hpp>
#include <das/Utils/Expected.h>
#include <unordered_map>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class ErrorLensManager
{
private:
    std::unordered_map<DasGuid, DasPtr<PluginInterface::IDasErrorLens>> map_{};

public:
    DasResult Register(
        Das::ExportInterface::IDasReadOnlyGuidVector* p_guid_vector,
        PluginInterface::IDasErrorLens*               p_error_lens);

    DasResult FindInterface(
        const DasGuid&                   iid,
        PluginInterface::IDasErrorLens** pp_out_lens);

    auto GetErrorMessage(
        const DasGuid&      iid,
        IDasReadOnlyString* locale_name,
        DasResult           error_code) const
        -> DAS::Utils::Expected<DasPtr<IDasReadOnlyString>>;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_ERRORLENSMANAGER_H
