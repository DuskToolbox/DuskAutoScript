#include <cstdint>
#include <cstring>
#include <das/Core/IPC/MemorySerializer.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using DAS::Core::IPC::MemorySerializerReader;
using DAS::Core::IPC::MemorySerializerWriter;

// Test basic integer types
TEST(MemorySerializerTest, WriteReadInt8)
{
    MemorySerializerWriter writer;
    int8_t                 value = -42;
    EXPECT_EQ(writer.WriteInt8(value), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    int8_t                 read_value;
    EXPECT_EQ(reader.ReadInt8(&read_value), DAS_S_OK);
    EXPECT_EQ(read_value, value);
}

TEST(MemorySerializerTest, WriteReadUInt8)
{
    MemorySerializerWriter writer;
    uint8_t                value = 255;
    EXPECT_EQ(writer.WriteUInt8(value), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    uint8_t                read_value;
    EXPECT_EQ(reader.ReadUInt8(&read_value), DAS_S_OK);
    EXPECT_EQ(read_value, value);
}

TEST(MemorySerializerTest, WriteReadInt16)
{
    MemorySerializerWriter writer;
    int16_t                value = -1000;
    EXPECT_EQ(writer.WriteInt16(value), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    int16_t                read_value;
    EXPECT_EQ(reader.ReadInt16(&read_value), DAS_S_OK);
    EXPECT_EQ(read_value, value);
}

TEST(MemorySerializerTest, WriteReadInt32)
{
    MemorySerializerWriter writer;
    int32_t                value = -1234567;
    EXPECT_EQ(writer.WriteInt32(value), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    int32_t                read_value;
    EXPECT_EQ(reader.ReadInt32(&read_value), DAS_S_OK);
    EXPECT_EQ(read_value, value);
}

// Test floating point types
TEST(MemorySerializerTest, WriteReadFloat)
{
    MemorySerializerWriter writer;
    float                  value = 3.14159f;
    EXPECT_EQ(writer.WriteFloat(value), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    float                  read_value;
    EXPECT_EQ(reader.ReadFloat(&read_value), DAS_S_OK);
    EXPECT_FLOAT_EQ(read_value, value);
}

TEST(MemorySerializerTest, WriteReadDouble)
{
    MemorySerializerWriter writer;
    double                 value = 2.718281828459045;
    EXPECT_EQ(writer.WriteDouble(value), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    double                 read_value;
    EXPECT_EQ(reader.ReadDouble(&read_value), DAS_S_OK);
    EXPECT_DOUBLE_EQ(read_value, value);
}

// Test boolean type
TEST(MemorySerializerTest, WriteReadBool)
{
    MemorySerializerWriter writer;
    EXPECT_EQ(writer.WriteBool(true), DAS_S_OK);
    EXPECT_EQ(writer.WriteBool(false), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    bool                   read_value1, read_value2;
    EXPECT_EQ(reader.ReadBool(&read_value1), DAS_S_OK);
    EXPECT_EQ(reader.ReadBool(&read_value2), DAS_S_OK);
    EXPECT_TRUE(read_value1);
    EXPECT_FALSE(read_value2);
}

// Test bytes type
TEST(MemorySerializerTest, WriteReadBytes)
{
    MemorySerializerWriter writer;
    std::vector<uint8_t>   data = {1, 2, 3, 4, 5};
    EXPECT_EQ(writer.WriteBytes(data.data(), data.size()), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    std::vector<uint8_t>   read_data;
    EXPECT_EQ(reader.ReadBytes(read_data), DAS_S_OK);
    EXPECT_EQ(read_data, data);
}

// Test string type
TEST(MemorySerializerTest, WriteReadString)
{
    MemorySerializerWriter writer;
    const char*            str = "Hello, World!";
    size_t                 length = std::strlen(str);
    EXPECT_EQ(writer.WriteString(str, length), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    std::string            read_str;
    EXPECT_EQ(reader.ReadString(read_str), DAS_S_OK);
    EXPECT_EQ(read_str, str);
}

// Test empty buffer
TEST(MemorySerializerTest, ReadEmptyBuffer)
{
    std::vector<uint8_t>   empty_buffer;
    MemorySerializerReader reader(empty_buffer);

    int8_t value;
    EXPECT_NE(reader.ReadInt8(&value), DAS_S_OK);
}

// Test seek functionality
TEST(MemorySerializerTest, SeekAndRead)
{
    MemorySerializerWriter writer;
    writer.WriteInt8(1);
    writer.WriteInt8(2);
    writer.WriteInt8(3);

    MemorySerializerReader reader(writer.GetBuffer());
    int8_t                 value;

    EXPECT_EQ(reader.ReadInt8(&value), DAS_S_OK);
    EXPECT_EQ(value, 1);

    EXPECT_EQ(reader.Seek(0), DAS_S_OK);
    EXPECT_EQ(reader.ReadInt8(&value), DAS_S_OK);
    EXPECT_EQ(value, 1);

    EXPECT_EQ(reader.Seek(2), DAS_S_OK);
    EXPECT_EQ(reader.ReadInt8(&value), DAS_S_OK);
    EXPECT_EQ(value, 3);
}

// Test buffer position tracking
TEST(MemorySerializerTest, PositionTracking)
{
    MemorySerializerWriter writer;
    EXPECT_EQ(writer.GetPosition(), 0);

    writer.WriteInt8(1);
    EXPECT_EQ(writer.GetPosition(), 1);

    writer.WriteInt32(0x12345678);
    EXPECT_EQ(writer.GetPosition(), 5);

    writer.WriteFloat(1.0f);
    EXPECT_EQ(writer.GetPosition(), 9);
}

// Test remaining bytes calculation
TEST(MemorySerializerTest, RemainingBytesCalculation)
{
    MemorySerializerWriter writer;
    writer.WriteInt8(1);
    writer.WriteInt8(2);
    writer.WriteInt8(3);

    MemorySerializerReader reader(writer.GetBuffer());
    EXPECT_EQ(reader.GetRemaining(), 3);

    int8_t value;
    reader.ReadInt8(&value);
    EXPECT_EQ(reader.GetRemaining(), 2);

    reader.ReadInt8(&value);
    EXPECT_EQ(reader.GetRemaining(), 1);

    reader.ReadInt8(&value);
    EXPECT_EQ(reader.GetRemaining(), 0);
}

// Test buffer functionality
TEST(MemorySerializerTest, BufferOperations)
{
    MemorySerializerWriter writer;

    // Test Clear
    EXPECT_TRUE(writer.IsEmpty());
    writer.WriteInt8(42);
    EXPECT_FALSE(writer.IsEmpty());
    writer.Clear();
    EXPECT_TRUE(writer.IsEmpty());
    EXPECT_EQ(writer.Size(), 0);

    // Test Size
    writer.WriteUInt32(100);
    writer.WriteDouble(3.14);
    EXPECT_EQ(writer.Size(), 12);
}

// Test pre-allocated buffer
TEST(MemorySerializerTest, PreAllocatedBuffer)
{
    MemorySerializerWriter writer(1024);
    EXPECT_GE(writer.GetBuffer().capacity(), 1024);

    writer.WriteInt32(123456);
    EXPECT_EQ(writer.Size(), 4);
    EXPECT_FALSE(writer.IsEmpty());
}

// Test Reader with direct buffer
TEST(MemorySerializerTest, ReaderDirectBuffer)
{
    std::vector<uint8_t>   data = {0x01, 0x02, 0x03, 0x04};
    MemorySerializerReader reader(data.data(), data.size());

    uint8_t value;
    EXPECT_EQ(reader.ReadUInt8(&value), DAS_S_OK);
    EXPECT_EQ(value, 0x01);

    EXPECT_EQ(reader.Seek(3), DAS_S_OK);
    EXPECT_EQ(reader.ReadUInt8(&value), DAS_S_OK);
    EXPECT_EQ(value, 0x04);
}

// Test error cases
TEST(MemorySerializerTest, ErrorCases)
{
    // Test seeking beyond buffer size in writer
    MemorySerializerWriter writer;
    writer.WriteInt8(1);
    EXPECT_EQ(writer.Seek(10), DAS_E_IPC_DESERIALIZATION_FAILED);

    // Test reading beyond buffer size in reader
    std::vector<uint8_t>   small_data = {0x01};
    MemorySerializerReader reader(small_data);

    int16_t value;
    EXPECT_EQ(reader.ReadInt16(&value), DAS_E_IPC_DESERIALIZATION_FAILED);

    // Test seeking beyond buffer size in reader
    EXPECT_EQ(reader.Seek(10), DAS_E_IPC_DESERIALIZATION_FAILED);
}