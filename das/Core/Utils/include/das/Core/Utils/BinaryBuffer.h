#ifndef DAS_CORE_UTILS_BINARYBUFFER_H
#define DAS_CORE_UTILS_BINARYBUFFER_H

#include <das/_autogen/idl/abi/IDasBinaryBuffer.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasBinaryBuffer.Implements.hpp>

#include <cstdint>
#include <das/Utils/CommonUtils.hpp>
#include <memory>

namespace DAS::Core::Utils
{

    class DasBinaryBufferImpl final
        : public DAS::ExportInterface::DasBinaryBufferImplBase<
              DasBinaryBufferImpl>
    {
    public:
        explicit DasBinaryBufferImpl(const size_t size_in_bytes)
            : size_{size_in_bytes}
        {
            up_data_ = std::make_unique<unsigned char[]>(size_in_bytes);
        }

        DAS_IMPL GetData(unsigned char** pp_out_data) override
        {
            DAS_UTILS_CHECK_POINTER(pp_out_data);
            *pp_out_data = up_data_.get();
            return DAS_S_OK;
        }

        DAS_IMPL GetSize(uint64_t* p_out_size) override
        {
            DAS_UTILS_CHECK_POINTER(p_out_size);
            *p_out_size = size_;
            return DAS_S_OK;
        }

    private:
        size_t                           size_;
        std::unique_ptr<unsigned char[]> up_data_;
    };

} // namespace DAS::Core::Utils

#endif // DAS_CORE_UTILS_BINARYBUFFER_H
