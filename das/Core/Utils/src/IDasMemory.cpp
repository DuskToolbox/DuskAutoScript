#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/BinaryBuffer.h>
#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasMemory.Implements.hpp>
#include <memory>

namespace
{
    class DasMemoryImpl;

    class WholeBufferView final : public DAS::ExportInterface::IDasBinaryBuffer
    {
    public:
        explicit WholeBufferView(DasMemoryImpl& owner) : owner_{owner} {}

        uint32_t DAS_STD_CALL AddRef() override;
        uint32_t DAS_STD_CALL Release() override;
        DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_out_object)
            override;
        DAS_IMPL GetData(unsigned char** pp_out_data) override;
        DAS_IMPL GetSize(uint64_t* p_out_size) override;

    private:
        DasMemoryImpl& owner_;
    };

    class DasMemoryBinaryBufferImpl final
        : public DAS::ExportInterface::DasBinaryBufferImplBase<
              DasMemoryBinaryBufferImpl>
    {
    public:
        DasMemoryBinaryBufferImpl(
            DAS::ExportInterface::IDasMemory* p_memory,
            uint64_t                          offset)
            : p_memory_{p_memory}, offset_{offset}
        {
        }

        DAS_IMPL GetData(unsigned char** pp_out_data) override;
        DAS_IMPL GetSize(uint64_t* p_out_size) override;

    private:
        DAS::DasPtr<DAS::ExportInterface::IDasMemory> p_memory_;
        uint64_t                                      offset_;
    };

    class DasMemoryImpl final
        : public DAS::ExportInterface::DasMemoryImplBase<DasMemoryImpl>
    {
        friend class DasMemoryBinaryBufferImpl;
        friend class WholeBufferView;

    public:
        explicit DasMemoryImpl(const size_t size_in_bytes)
            : size_{size_in_bytes},
              up_data_{std::make_unique<unsigned char[]>(size_in_bytes)},
              whole_buffer_view_{*this}
        {
        }

        DAS_IMPL GetBinaryBuffer(
            uint64_t                                 offset,
            DAS::ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
        {
            return CreateView(offset, pp_out_buffer);
        }

        DAS_IMPL GetMutableView(
            uint64_t                                 offset,
            DAS::ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
        {
            return CreateView(offset, pp_out_buffer);
        }

        DAS_IMPL GetSize(uint64_t* p_out_size) override
        {
            DAS_UTILS_CHECK_POINTER(p_out_size);
            *p_out_size = size_;
            return DAS_S_OK;
        }

    private:
        DAS_IMPL CreateView(
            uint64_t                                 offset,
            DAS::ExportInterface::IDasBinaryBuffer** pp_out_buffer)
        {
            DAS_UTILS_CHECK_POINTER(pp_out_buffer);
            *pp_out_buffer = nullptr;

            if (offset > size_) [[unlikely]]
            {
                DAS_CORE_LOG_ERROR(
                    "IDasMemory view offset out of range: offset={}, size={}, "
                    "result={}",
                    offset,
                    size_,
                    DAS_E_OUT_OF_RANGE);
                return DAS_E_OUT_OF_RANGE;
            }

            if (offset == 0)
            {
                return whole_buffer_view_.QueryInterface(
                    DasIidOf<DAS::ExportInterface::IDasBinaryBuffer>(),
                    reinterpret_cast<void**>(pp_out_buffer));
            }

            try
            {
                *pp_out_buffer =
                    DasMemoryBinaryBufferImpl::MakeRaw(this, offset);
                return DAS_S_OK;
            }
            catch (std::bad_alloc&)
            {
                return DAS_E_OUT_OF_MEMORY;
            }
        }

        [[nodiscard]]
        unsigned char* Data() noexcept
        {
            return up_data_.get();
        }

        [[nodiscard]]
        size_t Size() const noexcept
        {
            return size_;
        }

        size_t                           size_;
        std::unique_ptr<unsigned char[]> up_data_;
        WholeBufferView                  whole_buffer_view_;
    };

    uint32_t WholeBufferView::AddRef() { return owner_.AddRef(); }

    uint32_t WholeBufferView::Release() { return owner_.Release(); }

    DAS_IMPL WholeBufferView::QueryInterface(
        const DasGuid& iid,
        void**         pp_out_object)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_object);

        if (iid == DasIidOf<IDasBase>())
        {
            *pp_out_object = static_cast<IDasBase*>(this);
        }
        else if (iid == DasIidOf<DAS::ExportInterface::IDasBinaryBuffer>())
        {
            *pp_out_object =
                static_cast<DAS::ExportInterface::IDasBinaryBuffer*>(this);
        }
        else
        {
            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        AddRef();
        return DAS_S_OK;
    }

    DAS_IMPL WholeBufferView::GetData(unsigned char** pp_out_data)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_data);
        *pp_out_data = owner_.Data();
        return DAS_S_OK;
    }

    DAS_IMPL WholeBufferView::GetSize(uint64_t* p_out_size)
    {
        DAS_UTILS_CHECK_POINTER(p_out_size);
        *p_out_size = owner_.Size();
        return DAS_S_OK;
    }

    DAS_IMPL DasMemoryBinaryBufferImpl::GetData(unsigned char** pp_out_data)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_data);
        *pp_out_data =
            static_cast<DasMemoryImpl*>(p_memory_.Get())->Data() + offset_;
        return DAS_S_OK;
    }

    DAS_IMPL DasMemoryBinaryBufferImpl::GetSize(uint64_t* p_out_size)
    {
        DAS_UTILS_CHECK_POINTER(p_out_size);
        const auto* const p_memory =
            static_cast<const DasMemoryImpl*>(p_memory_.Get());
        *p_out_size = p_memory->Size() - offset_;
        return DAS_S_OK;
    }
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
