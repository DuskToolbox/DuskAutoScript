#ifndef DAS_CORE_ORTWRAPPER_IDASREADONLYSTRINGVECTORIMPL_H
#define DAS_CORE_ORTWRAPPER_IDASREADONLYSTRINGVECTORIMPL_H

#include "Config.h"
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasReadOnlyStringVector.Implements.hpp>
#include <string>
#include <vector>

// {57FE1158-C31C-4804-83B4-2AC49AEA6333}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OrtWrapper,
    IDasReadOnlyStringVectorImpl,
    0x57fe1158,
    0xc31c,
    0x4804,
    0x83,
    0xb4,
    0x2a,
    0xc4,
    0x9a,
    0xea,
    0x63,
    0x33);

DAS_CORE_ORTWRAPPER_NS_BEGIN

class IDasReadOnlyStringVectorImpl final
    : public Das::ExportInterface::DasReadOnlyStringVectorImplBase<
          IDasReadOnlyStringVectorImpl>
{
    std::vector<Das::DasPtr<IDasReadOnlyString>> items_;

public:
    IDasReadOnlyStringVectorImpl() = default;

    explicit IDasReadOnlyStringVectorImpl(std::vector<std::string> strings);

    DAS_IMPL GetCount(uint32_t* p_count) override;
    DAS_IMPL GetAt(uint32_t index, IDasReadOnlyString** pp_out_value) override;

    void AddString(const char* str);
    void Reserve(size_t count);
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_IDASREADONLYSTRINGVECTORIMPL_H
