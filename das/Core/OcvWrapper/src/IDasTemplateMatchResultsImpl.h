#ifndef DAS_CORE_OCVWRAPPER_IDASTEMPLATEMATCHRESULTSIMPL_H
#define DAS_CORE_OCVWRAPPER_IDASTEMPLATEMATCHRESULTSIMPL_H

#include <das/Core/OcvWrapper/Config.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/DasBasicTypes.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasTemplateMatchResults.Implements.hpp>
#include <vector>

// {E1A2B3C4-D5E6-4F7A-8B9C-0D1E2F3A4B5C}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OcvWrapper,
    IDasTemplateMatchResultsImpl,
    0xe1a2b3c4,
    0xd5e6,
    0x4f7a,
    0x8b,
    0x9c,
    0x0d,
    0x1e,
    0x2f,
    0x3a,
    0x4b,
    0x5c);

DAS_CORE_OCVWRAPPER_NS_BEGIN

class IDasTemplateMatchResultsImpl final
    : public ExportInterface::DasTemplateMatchResultsImplBase<
          IDasTemplateMatchResultsImpl>
{
    std::vector<DasPtr<ExportInterface::IDasTemplateMatchResult>> results_;
    uint32_t raw_match_count_{};

public:
    IDasTemplateMatchResultsImpl() = default;

    DAS_IMPL GetCount(uint32_t* p_out_count) override;
    DAS_IMPL GetRawMatchCount(uint32_t* p_out_raw_count) override;
    DAS_IMPL GetAt(
        uint32_t                                   index,
        ExportInterface::IDasTemplateMatchResult** pp_out_result) override;

    void AddResult(ExportInterface::IDasTemplateMatchResult* p_result);
    void SetRawMatchCount(uint32_t count);
    void Reserve(size_t count);
};

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_IDASTEMPLATEMATCHRESULTSIMPL_H
