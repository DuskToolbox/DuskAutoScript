#include <DAS/_autogen/idl/wrapper/Das.ExportInterface.IDasMemory.Implements.hpp>
#include <cstring>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>
#include <memory>

namespace
{
    class DasMemoryImpl final
        : public DAS::ExportInterface::DasMemoryImplBase<DasMemoryImpl>
    {
    public:
        DasMemoryImpl(const size_t size_in_bytes) : size{size_in_bytes}
        {
            up_data_ = std::make_unique<unsigned char[]>(size_in_bytes);
            p_offset_data_ = up_data_.get();
        }

        DAS_IMPL GetRawData(unsigned char** pp_out_data) override
        {
            DAS_UTILS_CHECK_POINTER(pp_out_data);

            *pp_out_data = up_data_.get();
            return DAS_S_OK;
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
            DAS_UTILS_CHECK_POINTER(p_out_offset)

            *p_out_offset = p_offset_data_ - up_data_.get();

            return DAS_S_OK;
        }

        DAS_IMPL Resize(size_t new_size_in_byte) override
        {
            if (new_size_in_byte > size)
            {
                auto up_new_data = std::make_unique<unsigned char[]>(size);

                std::memcpy(up_new_data.get(), up_data_.get(), size);

                up_data_ = std::move(up_new_data);
                size = new_size_in_byte;

                return DAS_S_OK;
            }
            return DAS_S_FALSE;
        }

    private:
        unsigned char*                   p_offset_data_;
        size_t                           size;
        std::unique_ptr<unsigned char[]> up_data_{};
    };
} // namespace

DasResult CreateIDasMemory(
    size_t                             size_in_byte,
    Das::ExportInterface::IDasMemory** pp_out_memory)
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
