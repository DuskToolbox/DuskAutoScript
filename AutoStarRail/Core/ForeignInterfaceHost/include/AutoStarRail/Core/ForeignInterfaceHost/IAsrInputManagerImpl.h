#ifndef ASR_CORE_FOREIGNINTERFACEHOST_IASRINPUTMANAGERIMPL_H
#define ASR_CORE_FOREIGNINTERFACEHOST_IASRINPUTMANAGERIMPL_H

#include <AutoStarRail/Core/ForeignInterfaceHost/Config.h>
#include <AutoStarRail/ExportInterface/IAsrInputManager.h>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class IAsrInputManagerImpl final : public IAsrInputManager
{
    ASR_METHOD EnumErrorState(
        size_t               index,
        AsrResult*           p_error_code,
        IAsrReadOnlyString** pp_out_error_explanation) = 0;
};

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // ASR_CORE_FOREIGNINTERFACEHOST_IASRINPUTMANAGERIMPL_H
