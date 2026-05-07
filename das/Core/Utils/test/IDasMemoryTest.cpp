#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasBinaryBuffer.h>
#include <das/_autogen/idl/abi/IDasMemory.h>
#include <gtest/gtest.h>

namespace
{
    unsigned char* GetData(DAS::ExportInterface::IDasBinaryBuffer* buffer)
    {
        unsigned char* data = nullptr;
        EXPECT_EQ(buffer->GetData(&data), DAS_S_OK);
        EXPECT_NE(data, nullptr);
        return data;
    }
} // namespace

TEST(IDasMemoryTest, GetBinaryBufferReturnsZeroCopyView)
{
    DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
    ASSERT_EQ(::CreateIDasMemory(16, memory.Put()), DAS_S_OK);

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> first_buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(first_buffer.Put()), DAS_S_OK);
    auto* first_data = GetData(first_buffer.Get());
    first_data[3] = 42;

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> second_buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(second_buffer.Put()), DAS_S_OK);
    auto* second_data = GetData(second_buffer.Get());

    EXPECT_EQ(second_data, first_data);
    EXPECT_EQ(second_data[3], 42);
}

TEST(IDasMemoryTest, GetBinaryBufferRespectsOffset)
{
    DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
    ASSERT_EQ(::CreateIDasMemory(16, memory.Put()), DAS_S_OK);

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> base_buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(base_buffer.Put()), DAS_S_OK);
    auto* base_data = GetData(base_buffer.Get());

    ASSERT_EQ(memory->SetOffset(4), DAS_S_OK);

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> offset_buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(offset_buffer.Put()), DAS_S_OK);
    auto* offset_data = GetData(offset_buffer.Get());

    uint64_t offset_size = 0;
    ASSERT_EQ(offset_buffer->GetSize(&offset_size), DAS_S_OK);

    EXPECT_EQ(offset_data, base_data + 4);
    EXPECT_EQ(offset_size, 12);
}

TEST(IDasMemoryTest, SetOffsetRejectsNegativeOffset)
{
    DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
    ASSERT_EQ(::CreateIDasMemory(16, memory.Put()), DAS_S_OK);

    EXPECT_EQ(memory->SetOffset(-1), DAS_E_OUT_OF_RANGE);
}
