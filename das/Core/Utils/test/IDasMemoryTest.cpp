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

TEST(IDasMemoryTest, GetMutableViewSharesBackingWithReadView)
{
    DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
    ASSERT_EQ(::CreateIDasMemory(16, memory.Put()), DAS_S_OK);

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> mutable_view;
    ASSERT_EQ(memory->GetMutableView(4, mutable_view.Put()), DAS_S_OK);
    auto* mutable_data = GetData(mutable_view.Get());
    mutable_data[0] = 51;
    mutable_data[3] = 99;

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> read_view;
    ASSERT_EQ(memory->GetBinaryBuffer(4, read_view.Put()), DAS_S_OK);
    auto* read_data = GetData(read_view.Get());

    uint64_t read_size = 0;
    ASSERT_EQ(read_view->GetSize(&read_size), DAS_S_OK);

    EXPECT_EQ(read_data, mutable_data);
    EXPECT_EQ(read_data[0], 51);
    EXPECT_EQ(read_data[3], 99);
    EXPECT_EQ(read_size, 12);
}

TEST(IDasMemoryTest, ViewKeepsBackingAliveAfterMemoryRelease)
{
    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> retained_offset_view;
    {
        DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
        ASSERT_EQ(::CreateIDasMemory(16, memory.Put()), DAS_S_OK);

        DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> seeder;
        ASSERT_EQ(memory->GetBinaryBuffer(0, seeder.Put()), DAS_S_OK);
        auto* seed_data = GetData(seeder.Get());
        seed_data[4] = 21;
        seed_data[7] = 84;

        ASSERT_EQ(
            memory->GetBinaryBuffer(4, retained_offset_view.Put()),
            DAS_S_OK);
    }

    uint64_t offset_size = 0;
    ASSERT_EQ(retained_offset_view->GetSize(&offset_size), DAS_S_OK);
    auto* offset_data = GetData(retained_offset_view.Get());

    EXPECT_EQ(offset_size, 12);
    EXPECT_EQ(offset_data[0], 21);
    EXPECT_EQ(offset_data[3], 84);

    DAS::DasPtr<DAS::ExportInterface::IDasBinaryBuffer> retained_whole_view;
    {
        DAS::DasPtr<DAS::ExportInterface::IDasMemory> memory;
        ASSERT_EQ(::CreateIDasMemory(8, memory.Put()), DAS_S_OK);
        ASSERT_EQ(
            memory->GetBinaryBuffer(0, retained_whole_view.Put()),
            DAS_S_OK);

        auto* whole_data = GetData(retained_whole_view.Get());
        whole_data[0] = 13;
        whole_data[7] = 55;
    }

    uint64_t whole_size = 0;
    ASSERT_EQ(retained_whole_view->GetSize(&whole_size), DAS_S_OK);
    auto* whole_data = GetData(retained_whole_view.Get());

    EXPECT_EQ(whole_size, 8);
    EXPECT_EQ(whole_data[0], 13);
    EXPECT_EQ(whole_data[7], 55);
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
