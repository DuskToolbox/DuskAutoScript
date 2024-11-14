#ifndef DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTMANAGERIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTMANAGERIMPL_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/ExportInterface/IDasInputManager.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class IDasInputManagerImpl final : public IDasInputManager
{
    DAS_METHOD EnumErrorState(
        size_t               index,
        DasResult*           p_error_code,
        IDasReadOnlyString** pp_out_error_explanation) = 0;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_IDASINPUTMANAGERIMPL_H
