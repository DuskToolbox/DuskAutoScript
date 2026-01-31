#include <cstring>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasMemory.Implements.hpp>
#include <memory>

#include <das/_autogen/idl/abi/IDasBinaryBuffer.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasBinaryBuffer.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasBinaryBuffer.hpp>

namespace
{
    class DasBinaryBufferImpl final
        : public DAS::ExportInterface::DasBinaryBufferImplBase<
              DasBinaryBufferImpl>
    {
    public:
        explicit DasBinaryBufferImpl(const size_t size) : size_{size}
        {
            up_data_ = std::make_unique<unsigned char[]>(size);
        }

        DasResult GetData(unsigned char** pp_out_data) override
        {
            DAS_UTILS_CHECK_POINTER(pp_out_data);
            *pp_out_data = up_data_.get();
            return DAS_S_OK;
        }

        DasResult GetSize(uint64_t* p_out_size) override
        {
            DAS_UTILS_CHECK_POINTER(p_out_size);
            *p_out_size = size_;
            return DAS_S_OK;
        }

    private:
        size_t                           size_;
        std::unique_ptr<unsigned char[]> up_data_;
    };

    class DasMemoryImpl final
        : public DAS::ExportInterface::DasMemoryImplBase<DasMemoryImpl>
    {
    public:
        DasMemoryImpl(const size_t size_in_bytes) : size{size_in_bytes}
        {
            up_data_ = std::make_unique<unsigned char[]>(size_in_bytes);
            p_offset_data_ = up_data_.get();
        }

        DAS_IMPL GetBinaryBuffer(
            DAS::ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
        {
            DAS_UTILS_CHECK_POINTER(pp_out_buffer);

            try
            {
                auto* const    p_buffer = DasBinaryBufferImpl::MakeRaw(size);
                unsigned char* p_buffer_data{};
                const auto get_data_result = p_buffer->GetData(&p_buffer_data);
                if (DAS::IsFailed(get_data_result))
                {
                    p_buffer->Release();
                    return get_data_result;
                }

                std::memcpy(p_buffer_data, up_data_.get(), size);

                *pp_out_buffer = p_buffer;
                return DAS_S_OK;
            }
            catch (std::bad_alloc&)
            {
                return DAS_E_OUT_OF_MEMORY;
            }
        }

        DAS_IMPL SetOffset(ptrdiff_t offset) override
        {
            if (const auto ptrdiff_t_size = static_cast<ptrdiff_t>(size);
                offset < ptrdiff_t_size) [[likely]]
            {
                p_offset_data_ = up_data_.get() + offset;
                return DAS_S_OK;
            }

            DAS_CORE_LOG_ERROR(
                "Invalid offset detected: input {} should be less than {}.",
                offset,
                size);

            return DAS_E_OUT_OF_RANGE;
        }

        DAS_IMPL GetOffset(ptrdiff_t* p_out_offset) override
        {
            DAS_UTILS_CHECK_POINTER(p_out_offset);

            *p_out_offset = p_offset_data_ - up_data_.get();

            return DAS_S_OK;
        }

        DAS_IMPL Resize(size_t new_size_in_byte) override
        {
            if (new_size_in_byte > size)
            {
                auto up_new_data =
                    std::make_unique<unsigned char[]>(new_size_in_byte);

                std::memcpy(up_new_data.get(), up_data_.get(), size);

                up_data_ = std::move(up_new_data);
                size = new_size_in_byte;
                p_offset_data_ = up_data_.get();

                return DAS_S_OK;
            }
            return DAS_S_FALSE;
        }

        DAS_IMPL GetSize(uint64_t* p_out_size) override
        {
            DAS_UTILS_CHECK_POINTER(p_out_size);
            *p_out_size = size;
            return DAS_S_OK;
        }

    private:
        unsigned char*                   p_offset_data_;
        size_t                           size;
        std::unique_ptr<unsigned char[]> up_data_;
    };
} // namespace

DasResult CreateIDasMemory(
    size_t                             size_in_byte,
    DAS::ExportInterface::IDasMemory** pp_out_memory)
{
    DAS_UTILS_CHECK_POINTER(pp_out_memory)

    try
    {
        auto* const p_memory = new DasMemoryImpl(size_in_byte);
        p_memory->AddRef();
        *pp_out_memory = p_memory;
        return DAS_S_OK;
    }
    catch (std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}
