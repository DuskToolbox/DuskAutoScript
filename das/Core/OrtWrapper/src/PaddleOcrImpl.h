#ifndef DAS_CORE_ORTWRAPPER_PADDLEOCRIMPL_H
#define DAS_CORE_ORTWRAPPER_PADDLEOCRIMPL_H

#include "DasOrt.h"

#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasAI.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasOcr.Implements.hpp>

#include <cstdint>
#include <string>
#include <vector>

// {B3D4E5F6-7890-4ABC-DEF0-123456789ABC}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OrtWrapper,
    PaddleOcrImpl,
    0xb3d4e5f6,
    0x7890,
    0x4abc,
    0xde,
    0xf0,
    0x12,
    0x34,
    0x56,
    0x78,
    0x9a,
    0xbc);

DAS_CORE_ORTWRAPPER_NS_BEGIN

class PaddleOcrImpl final
    : public Das::ExportInterface::DasOcrImplBase<PaddleOcrImpl>
{
    // Eat-dogfood: inference goes through IDasAI (D-v14-07)
    Das::DasPtr<Das::ExportInterface::IDasAI>      ai_;
    Das::DasPtr<Das::ExportInterface::IDasSession> det_session_;
    Das::DasPtr<Das::ExportInterface::IDasSession> rec_session_;
    std::vector<std::string>                       dict_;

    // Output names discovered at session construction (Pitfall 2)
    std::vector<std::string> det_output_names_;
    std::vector<std::string> rec_output_names_;

    // Input shape hints
    int64_t det_input_height_ = 960;
    int64_t det_input_width_ = 960;
    int64_t rec_input_height_ = 32;
    int64_t rec_input_width_ = 320;

public:
    PaddleOcrImpl(
        Das::ExportInterface::IDasAI*      ai,
        Das::ExportInterface::IDasSession* det_session,
        Das::ExportInterface::IDasSession* rec_session,
        std::vector<std::string>           dict);

    DAS_IMPL Recognize(
        Das::ExportInterface::IDasImage*            p_image,
        Das::ExportInterface::IDasOcrResultVector** pp_results) override;
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_PADDLEOCRIMPL_H
