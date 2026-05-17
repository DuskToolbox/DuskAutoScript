#ifndef DAS_CORE_ORTWRAPPER_AICUDAIMPL_H
#define DAS_CORE_ORTWRAPPER_AICUDAIMPL_H

#include "DasOrt.h"

#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasAI.Implements.hpp>

// {EDEC874A-0642-4756-B9F9-1EC8A2279B22}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OrtWrapper,
    AiCudaImpl,
    0xedec874a,
    0x0642,
    0x4756,
    0xb9,
    0xf9,
    0x1e,
    0xc8,
    0xa2,
    0x27,
    0x9b,
    0x22);

DAS_CORE_ORTWRAPPER_NS_BEGIN

class AiCudaImpl final : public Das::ExportInterface::DasAIImplBase<AiCudaImpl>,
                         public DasOrt
{
public:
    AiCudaImpl() : DasOrt("AiCudaImpl") {}

    DAS_IMPL CreateSession(
        IDasReadOnlyString*            model_path,
        ExportInterface::IDasJson*     options,
        ExportInterface::IDasSession** pp_session) override;

    DAS_IMPL CreateTensorFromImage(
        ExportInterface::IDasImage*                   image,
        const ExportInterface::DasImageTensorOptions& options,
        ExportInterface::IDasTensor**                 pp_tensor) override;

    DAS_IMPL CreateOcr(
        IDasReadOnlyString*        det_model,
        IDasReadOnlyString*        rec_model,
        IDasReadOnlyString*        dict,
        ExportInterface::IDasOcr** pp_ocr) override;
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_AICUDAIMPL_H
