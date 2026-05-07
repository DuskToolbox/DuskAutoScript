#ifndef DAS_CORE_ORTWRAPPER_IDASOCRRESULTIMPL_H
#define DAS_CORE_ORTWRAPPER_IDASOCRRESULTIMPL_H

#include "Config.h"

#include <das/_autogen/idl/abi/DasBasicTypes.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasOcrResult.Implements.hpp>

#include <string>
#include <vector>

// {E1E182FE-FB14-48F0-8EA8-4BDF34EB974D}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OrtWrapper,
    IDasOcrResultImpl,
    0xe1e182fe,
    0xfb14,
    0x48f0,
    0x8e,
    0xa8,
    0x4b,
    0xdf,
    0x34,
    0xeb,
    0x97,
    0x4d);

DAS_CORE_ORTWRAPPER_NS_BEGIN

class IDasOcrResultImpl final
    : public Das::ExportInterface::DasOcrResultImplBase<IDasOcrResultImpl>
{
    std::string                                text_;
    Das::ExportInterface::DasRect              box_{};
    double                                     score_{};
    std::vector<Das::ExportInterface::DasRect> char_boxes_;
    std::vector<double>                        char_scores_;

public:
    IDasOcrResultImpl(
        std::string                                text,
        Das::ExportInterface::DasRect              box,
        double                                     score,
        std::vector<Das::ExportInterface::DasRect> char_boxes,
        std::vector<double>                        char_scores);

    DAS_IMPL GetText(IDasReadOnlyString** pp_text) override;
    DAS_IMPL GetBox(Das::ExportInterface::DasRect* p_box) override;
    DAS_IMPL GetScore(double* p_score) override;
    DAS_IMPL GetCharCount(uint32_t* p_count) override;
    DAS_IMPL GetCharBox(uint32_t index, Das::ExportInterface::DasRect* p_box)
        override;
    DAS_IMPL GetCharScore(uint32_t index, double* p_score) override;
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_IDASOCRRESULTIMPL_H
