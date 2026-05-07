#ifndef DAS_CORE_ORTWRAPPER_IDASSESSIONIMPL_H
#define DAS_CORE_ORTWRAPPER_IDASSESSIONIMPL_H

#include "DasOrt.h"
#include "IDasTensorVectorImpl.h"

#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasSession.Implements.hpp>
#include <string>
#include <vector>

// {ECD5FFFF-0536-4D5F-80CC-A51971B8D5AA}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OrtWrapper,
    IDasSessionImpl,
    0xecd5ffff,
    0x0536,
    0x4d5f,
    0x80,
    0xcc,
    0xa5,
    0x19,
    0x71,
    0xb8,
    0xd5,
    0xaa);

DAS_CORE_ORTWRAPPER_NS_BEGIN

class IDasSessionImpl final
    : public Das::ExportInterface::DasSessionImplBase<IDasSessionImpl>
{
    Ort::Session                     session_;
    Ort::AllocatorWithDefaultOptions allocator_;
    std::vector<std::string>         input_names_;
    std::vector<std::string>         output_names_;

public:
    explicit IDasSessionImpl(Ort::Session session);

    DAS_IMPL Run(
        ExportInterface::IDasReadOnlyStringVector* input_names,
        ExportInterface::IDasTensorVector*         inputs,
        ExportInterface::IDasReadOnlyStringVector* output_names,
        ExportInterface::IDasTensorVector**        pp_outputs) override;
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_ORTWRAPPER_IDASSESSIONIMPL_H
