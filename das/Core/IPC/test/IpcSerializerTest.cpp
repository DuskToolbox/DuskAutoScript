#include <cstdint>
#include <cstring>
#include <das/Core/IPC/Serializer.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using DAS::Core::IPC::SerializerReader;
using DAS::Core::IPC::SerializerWriter;

class MemorySerializerWriter : public SerializerWriter
{
private:
    std::vector<uint8_t> buffer_;

public:
    DasResult Write(const void* data, size_t size) override
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + size);
        return DAS_S_OK;
    }

    size_t GetPosition() const override { return buffer_.size(); }

    DasResult Seek(size_t position) override
    {
        if (position > buffer_.size())
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        buffer_.resize(position);
        return DAS_S_OK;
    }

    DasResult Reserve(size_t size) override
    {
        buffer_.reserve(buffer_.size() + size);
        return DAS_S_OK;
    }

    const std::vector<uint8_t>& GetBuffer() const { return buffer_; }

    void Clear() { buffer_.clear(); }
};

class MemorySerializerReader : public SerializerReader
{
private:
    std::vector<uint8_t> buffer_;
    size_t               position_;

public:
    explicit MemorySerializerReader(const std::vector<uint8_t>& buffer)
        : buffer_(buffer), position_(0)
    {
    }

    DasResult Read(void* data, size_t size) override
    {
        if (position_ + size > buffer_.size())
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        uint8_t* bytes = static_cast<uint8_t*>(data);
        std::memcpy(bytes, buffer_.data() + position_, size);
        position_ += size;
        return DAS_S_OK;
    }

    size_t GetPosition() const override { return position_; }

    size_t GetRemaining() const override { return buffer_.size() - position_; }

    DasResult Seek(size_t position) override
    {
        if (position > buffer_.size())
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        position_ = position;
        return DAS_S_OK;
    }
};

// Test basic integer types
TEST(IpcSerializerTest, WriteReadInt8)
{
    MemorySerializerWriter writer;
    int8_t                 value = -42;
    EXPECT_EQ(writer.WriteInt8(value), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    int8_t                 read_value;
    EXPECT_EQ(reader.ReadInt8(&read_value), DAS_S_OK);
    EXPECT_EQ(read_value, value);
}

TEST(IpcSerializerTest, WriteReadUInt8)
{
    MemorySerializerWriter writer;
    uint8_t                value = 255;
    EXPECT_EQ(writer.WriteUInt8(value), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    uint8_t                read_value;
    EXPECT_EQ(reader.ReadUInt8(&read_value), DAS_S_OK);
    EXPECT_EQ(read_value, value);
}

TEST(IpcSerializerTest, WriteReadInt16)
{
    MemorySerializerWriter writer;
    int16_t                value = -1000;
    EXPECT_EQ(writer.WriteInt16(value), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    int16_t                read_value;
    EXPECT_EQ(reader.ReadInt16(&read_value), DAS_S_OK);
    EXPECT_EQ(read_value, value);
}

TEST(IpcSerializerTest, WriteReadInt32)
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
TEST(IpcSerializerTest, WriteReadFloat)
{
    MemorySerializerWriter writer;
    float                  value = 3.14159f;
    EXPECT_EQ(writer.WriteFloat(value), DAS_S_OK);

    MemorySerializerReader reader(writer.GetBuffer());
    float                  read_value;
    EXPECT_EQ(reader.ReadFloat(&read_value), DAS_S_OK);
    EXPECT_FLOAT_EQ(read_value, value);
}

TEST(IpcSerializerTest, WriteReadDouble)
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
TEST(IpcSerializerTest, WriteReadBool)
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
TEST(IpcSerializerTest, WriteReadBytes)
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
TEST(IpcSerializerTest, WriteReadString)
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
TEST(IpcSerializerTest, ReadEmptyBuffer)
{
    std::vector<uint8_t>   empty_buffer;
    MemorySerializerReader reader(empty_buffer);

    int8_t value;
    EXPECT_NE(reader.ReadInt8(&value), DAS_S_OK);
}

// Test seek functionality
TEST(IpcSerializerTest, SeekAndRead)
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
TEST(IpcSerializerTest, PositionTracking)
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
TEST(IpcSerializerTest, RemainingBytesCalculation)
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
