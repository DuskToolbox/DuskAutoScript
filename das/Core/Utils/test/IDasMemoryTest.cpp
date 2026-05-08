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
    ASSERT_EQ(memory->GetBinaryBuffer(0, first_buffer.Put()), DAS_S_OK);
    auto* first_data = GetData(first_buffer.Get());
    first_data[3] = 42;

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> second_buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(0, second_buffer.Put()), DAS_S_OK);
    auto* second_data = GetData(second_buffer.Get());

    EXPECT_EQ(second_data, first_data);
    EXPECT_EQ(second_data[3], 42);
}

TEST(IDasMemoryTest, GetBinaryBufferOffsetZeroIsWholeStorageMigration)
{
    DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
    ASSERT_EQ(::CreateIDasMemory(16, memory.Put()), DAS_S_OK);

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(0, buffer.Put()), DAS_S_OK);
    auto* data = GetData(buffer.Get());
    data[15] = 42;

    uint64_t size = 0;
    ASSERT_EQ(buffer->GetSize(&size), DAS_S_OK);

    EXPECT_EQ(size, 16);
    EXPECT_EQ(data[15], 42);
}

TEST(IDasMemoryTest, GetBinaryBufferCreatesStableOffsetViews)
{
    DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
    ASSERT_EQ(::CreateIDasMemory(16, memory.Put()), DAS_S_OK);

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> base_buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(0, base_buffer.Put()), DAS_S_OK);
    auto* base_data = GetData(base_buffer.Get());
    base_data[4] = 17;
    base_data[8] = 42;

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> offset_four_buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(4, offset_four_buffer.Put()), DAS_S_OK);
    auto* offset_four_data = GetData(offset_four_buffer.Get());

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> offset_eight_buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(8, offset_eight_buffer.Put()), DAS_S_OK);
    auto* offset_eight_data = GetData(offset_eight_buffer.Get());

    uint64_t offset_four_size = 0;
    ASSERT_EQ(offset_four_buffer->GetSize(&offset_four_size), DAS_S_OK);

    uint64_t offset_eight_size = 0;
    ASSERT_EQ(offset_eight_buffer->GetSize(&offset_eight_size), DAS_S_OK);

    EXPECT_EQ(offset_four_data, base_data + 4);
    EXPECT_EQ(offset_four_data[0], 17);
    EXPECT_EQ(offset_four_size, 12);

    EXPECT_EQ(offset_eight_data, base_data + 8);
    EXPECT_EQ(offset_eight_data[0], 42);
    EXPECT_EQ(offset_eight_size, 8);
}

TEST(IDasMemoryTest, OffsetEqualSizeReturnsEmptyView)
{
    DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
    ASSERT_EQ(::CreateIDasMemory(16, memory.Put()), DAS_S_OK);

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> base_buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(0, base_buffer.Put()), DAS_S_OK);
    auto* base_data = GetData(base_buffer.Get());

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> empty_buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(16, empty_buffer.Put()), DAS_S_OK);

    uint64_t empty_size = 1;
    ASSERT_EQ(empty_buffer->GetSize(&empty_size), DAS_S_OK);

    auto* empty_data = GetData(empty_buffer.Get());
    EXPECT_EQ(empty_size, 0);
    EXPECT_EQ(empty_data, base_data + 16);
}

TEST(IDasMemoryTest, OffsetGreaterThanSizeReturnsOutOfRange)
{
    DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
    ASSERT_EQ(::CreateIDasMemory(16, memory.Put()), DAS_S_OK);

    auto* buffer =
        reinterpret_cast<DAS::ExportInterface::IDasBinaryBuffer*>(uintptr_t{1});
    EXPECT_EQ(memory->GetBinaryBuffer(17, &buffer), DAS_E_OUT_OF_RANGE);
    EXPECT_EQ(buffer, nullptr);

    buffer =
        reinterpret_cast<DAS::ExportInterface::IDasBinaryBuffer*>(uintptr_t{1});
    EXPECT_EQ(memory->GetMutableView(17, &buffer), DAS_E_OUT_OF_RANGE);
    EXPECT_EQ(buffer, nullptr);
}

TEST(IDasMemoryTest, NullOutPointerReturnsInvalidPointer)
{
    DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
    ASSERT_EQ(::CreateIDasMemory(16, memory.Put()), DAS_S_OK);

    EXPECT_EQ(memory->GetBinaryBuffer(0, nullptr), DAS_E_INVALID_POINTER);
    EXPECT_EQ(memory->GetMutableView(0, nullptr), DAS_E_INVALID_POINTER);

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> buffer;
    ASSERT_EQ(memory->GetBinaryBuffer(0, buffer.Put()), DAS_S_OK);

    EXPECT_EQ(buffer->GetData(nullptr), DAS_E_INVALID_POINTER);
    EXPECT_EQ(buffer->GetSize(nullptr), DAS_E_INVALID_POINTER);
}
