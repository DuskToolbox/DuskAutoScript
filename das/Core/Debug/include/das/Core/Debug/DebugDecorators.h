#ifndef DAS_CORE_DEBUG_DEBUGDECORATORS_H
#define DAS_CORE_DEBUG_DEBUGDECORATORS_H

#include <das/Core/Debug/Config.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/DasCV.h>
#include <das/_autogen/idl/abi/IDasCapture.h>
#include <das/_autogen/idl/abi/IDasInput.h>
#include <das/_autogen/idl/abi/IDasOcr.h>

DAS_CORE_DEBUG_NS_BEGIN

template <class TInterface>
class DebugDecorator;

Das::DasPtr<Das::ExportInterface::IDasCv> MaybeDecorateCvRaw(
    Das::DasPtr<Das::ExportInterface::IDasCv> cv,
    const char*                               p_service_name);

Das::DasPtr<Das::ExportInterface::IDasOcr> MaybeDecorateOcrRaw(
    Das::DasPtr<Das::ExportInterface::IDasOcr> ocr,
    const char*                                p_operation_name);

Das::DasPtr<Das::PluginInterface::IDasCapture> MaybeDecorateCapture(
    Das::DasPtr<Das::PluginInterface::IDasCapture> capture,
    const char*                                    p_capture_name);

Das::DasPtr<Das::PluginInterface::IDasInput> MaybeDecorateInput(
    Das::DasPtr<Das::PluginInterface::IDasInput> input,
    const char*                                  p_input_name);

Das::DasPtr<Das::PluginInterface::IDasInputFactory> MaybeDecorateInputFactory(
    Das::DasPtr<Das::PluginInterface::IDasInputFactory> factory,
    const char*                                         p_factory_name);

DAS_CORE_DEBUG_NS_END

#endif // DAS_CORE_DEBUG_DEBUGDECORATORS_H
