#ifndef DAS_CORE_OCVWRAPPER_IDASMATCHRESULTIMPL_H
#define DAS_CORE_OCVWRAPPER_IDASMATCHRESULTIMPL_H

#include "Config.h"

#include <das/_autogen/idl/abi/DasBasicTypes.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasCvMatchResult.Implements.hpp>
#include <vector>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OcvWrapper,
    IDasMatchResultImpl,
    0xd2e3f4a5,
    0xb6c7,
    0x4d8e,
    0x9f,
    0x0a,
    0x1b,
    0x2c,
    0x3d,
    0x4e,
    0x5f,
    0x6f);

DAS_CORE_OCVWRAPPER_NS_BEGIN

class IDasMatchResultImpl final
    : public ExportInterface::DasCvMatchResultImplBase<IDasMatchResultImpl>
{
    std::vector<ExportInterface::DasMatchedPoint> matches_;

public:
    IDasMatchResultImpl() = default;

    DAS_IMPL GetMatchCount(uint32_t* p_out_count) override;
    DAS_IMPL GetMatchPair(
        uint32_t                          index,
        ExportInterface::DasMatchedPoint* p_out_pair) override;

    void  AddMatch(const ExportInterface::DasMatchedPoint& match);
    void  Reserve(size_t count);
    auto& GetMatches() { return matches_; }
};

DAS_CORE_OCVWRAPPER_NS_END

#endif
