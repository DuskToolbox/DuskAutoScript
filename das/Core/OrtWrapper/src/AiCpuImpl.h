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

using Das::ExportInterface::IDasImage;
using Das::ExportInterface::IDasJson;
using Das::ExportInterface::IDasOcr;
using Das::ExportInterface::IDasReadOnlyString;
using Das::ExportInterface::IDasSession;
using Das::ExportInterface::IDasTensor;

class AiCpuImpl final : public Das::ExportInterface::DasAIImplBase<AiCpuImpl>,
                        public DasOrt
{
public:
    AiCpuImpl() : DasOrt("AiCpuImpl") {}

    DAS_IMPL CreateSession(
        IDasReadOnlyString* model_path,
        IDasJson*           options,
        IDasSession**       pp_session) override;

    DAS_IMPL CreateTensorFromImage(
        IDasImage*   image,
        int64_t*     shape,
        uint32_t     rank,
        double*      mean,
        double*      std,
        uint32_t     value_count,
        IDasTensor** pp_tensor) override;

    DAS_IMPL CreateOcr(
        IDasReadOnlyString* det_model,
        IDasReadOnlyString* rec_model,
        IDasReadOnlyString* dict,
        IDasOcr**           pp_ocr) override;
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_AICPUIMPL_H
