#ifndef DAS_MEMORY_H
#define DAS_MEMORY_H

#include <das/DasPtr.hpp>
#include <das/IDasBase.h>

// {0E8E6129-512E-4AAB-A4A6-327ADCACB019}
DAS_DEFINE_GUID(
    DAS_IID_MEMORY,
    IDasMemory,
    0xe8e6129,
    0x512e,
    0x4aab,
    0xa4,
    0xa6,
    0x32,
    0x7a,
    0xdc,
    0xac,
    0xb0,
    0x19);
SWIG_IGNORE(IDasMemory)
DAS_INTERFACE IDasMemory : public IDasBase
{
    DAS_METHOD GetData(unsigned char** pp_out_data) = 0;
    DAS_METHOD GetRawData(unsigned char** pp_out_data) = 0;
    DAS_METHOD GetSize(size_t * p_out_size) = 0;
    /**
     * @brief 如果确定输入的指针不是空指针，则返回值可以忽略
     * @param p_out_offset
     * @return
     */
    DAS_METHOD GetOffset(ptrdiff_t * p_out_offset) = 0;
    DAS_METHOD SetOffset(ptrdiff_t offset) = 0;
    DAS_METHOD Resize(size_t new_size_in_byte) = 0;
};

SWIG_IGNORE(CreateIDasMemory)
DAS_C_API DasResult
CreateIDasMemory(size_t size_in_byte, IDasMemory** pp_out_memory);

#ifdef __cplusplus

class DasMemory
{
    Das::DasPtr<IDasMemory> p_data_;

public:
    DasMemory(size_t size_in_bytes)
    {
        CreateIDasMemory(size_in_bytes, p_data_.Put());
    }

    ~DasMemory() = default;

    [[nodiscard]]
    unsigned char* GetData() const
    {
        unsigned char* p_data{nullptr};
        p_data_->GetData(&p_data);
        return p_data;
    }

    [[nodiscard]]
    size_t GetSize() const
    {
        size_t size{};
        p_data_->GetSize(&size);
        return size;
    }

    void SetBeginOffset(ptrdiff_t offset) { p_data_->SetOffset(offset); }

    // stl-like api
    unsigned char& operator[](size_t size_in_bytes)
    {
        return *(GetData() + size_in_bytes);
    }

    void resize(size_t new_size) { p_data_->Resize(new_size); }

    [[nodiscard]]
    IDasMemory* Get() const noexcept
    {
        return p_data_.Get();
    }
};

#endif // __cplusplus

#endif // DAS_MEMORY_H
