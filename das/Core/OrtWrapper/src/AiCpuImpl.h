#ifndef DAS_CORE_ORTWRAPPER_AICPUIMPL_H
#define DAS_CORE_ORTWRAPPER_AICPUIMPL_H

#include "DasOrt.h"

#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasAI.Implements.hpp>

// {2F528F8E-EF04-4251-9C78-27502280B68C}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OrtWrapper,
    AiCpuImpl,
    0x2f528f8e,
    0xef04,
    0x4251,
    0x9c,
    0x78,
    0x27,
    0x50,
    0x22,
    0x80,
    0xb6,
    0x8c);

DAS_CORE_ORTWRAPPER_NS_BEGIN

class AiCpuImpl final : public Das::ExportInterface::DasAIImplBase<AiCpuImpl>,
                        public DasOrt
{
public:
    AiCpuImpl() : DasOrt("AiCpuImpl") {}

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

// Shared CreateOcr implementation used by both AiCpuImpl and AiCudaImpl
DasResult CreateOcrImpl(
    ExportInterface::IDasAI*   ai,
    IDasReadOnlyString*        det_model,
    IDasReadOnlyString*        rec_model,
    IDasReadOnlyString*        dict_path,
    ExportInterface::IDasOcr** pp_ocr);

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_AICPUIMPL_H
