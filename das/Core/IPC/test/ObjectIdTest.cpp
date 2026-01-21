#include <das/Core/IPC/ObjectId.h>
#include <gtest/gtest.h>

using DAS::Core::IPC::ObjectId;

// Test basic encode/decode with non-zero values
TEST(ObjectIdTest, EncodeDecode_BasicValues)
{
    ObjectId original{.process_id = 1, .generation = 2, .local_id = 3};
    uint64_t encoded = EncodeObjectId(original);
    ObjectId decoded = DecodeObjectId(encoded);

    EXPECT_EQ(decoded.process_id, 1);
    EXPECT_EQ(decoded.generation, 2);
    EXPECT_EQ(decoded.local_id, 3);
}

// Test encode/decode with boundary values
TEST(ObjectIdTest, EncodeDecode_BoundaryValues)
{
    ObjectId max_values{
        .process_id = 0xFFFF,
        .generation = 0xFFFF,
        .local_id = 0xFFFFFFFF};
    uint64_t encoded = EncodeObjectId(max_values);
    ObjectId decoded = DecodeObjectId(encoded);

    EXPECT_EQ(decoded.process_id, 0xFFFF);
    EXPECT_EQ(decoded.generation, 0xFFFF);
    EXPECT_EQ(decoded.local_id, 0xFFFFFFFF);
}

// Test encode/decode with all zero values
TEST(ObjectIdTest, EncodeDecode_ZeroValues)
{
    ObjectId zero{.process_id = 0, .generation = 0, .local_id = 0};
    uint64_t encoded = EncodeObjectId(zero);
    ObjectId decoded = DecodeObjectId(encoded);

    EXPECT_EQ(encoded, 0);
    EXPECT_EQ(decoded.process_id, 0);
    EXPECT_EQ(decoded.generation, 0);
    EXPECT_EQ(decoded.local_id, 0);
}

// Test generation normal increment
TEST(ObjectIdTest, IncrementGeneration_NormalCase)
{
    EXPECT_EQ(IncrementGeneration(1), 2);
    EXPECT_EQ(IncrementGeneration(0xFFFE), 0xFFFF);
}

// Test generation overflow (0xFFFF -> 1)
TEST(ObjectIdTest, IncrementGeneration_Overflow)
{
    EXPECT_EQ(IncrementGeneration(0xFFFF), 1);
}

// Test IsValidObjectId with matching generation
TEST(ObjectIdTest, IsValidObjectId_MatchingGeneration)
{
    ObjectId obj{.generation = 5, .local_id = 100, .process_id = 1};
    EXPECT_TRUE(IsValidObjectId(obj, 5));
}

// Test IsValidObjectId with non-matching generation
TEST(ObjectIdTest, IsValidObjectId_NonMatchingGeneration)
{
    ObjectId obj{.generation = 5, .local_id = 100, .process_id = 1};
    EXPECT_FALSE(IsValidObjectId(obj, 10));
}

// Test IsNullObjectId with all-zero struct
TEST(ObjectIdTest, IsNullObjectId_Struct_AllZero)
{
    ObjectId zero{.process_id = 0, .generation = 0, .local_id = 0};
    EXPECT_TRUE(IsNullObjectId(zero));
}

// Test IsNullObjectId with non-zero struct
TEST(ObjectIdTest, IsNullObjectId_Struct_NonZero)
{
    ObjectId non_zero{.process_id = 1, .generation = 0, .local_id = 0};
    EXPECT_FALSE(IsNullObjectId(non_zero));
}

// Test IsNullObjectId with encoded zero
TEST(ObjectIdTest, IsNullObjectId_EncodedZero)
{
    EXPECT_TRUE(IsNullObjectId(static_cast<uint64_t>(0)));
}

// Test IsNullObjectId with encoded non-zero
TEST(ObjectIdTest, IsNullObjectId_EncodedNonZero)
{
    ObjectId obj{.process_id = 1, .generation = 0, .local_id = 0};
    uint64_t encoded = EncodeObjectId(obj);
    EXPECT_FALSE(IsNullObjectId(encoded));
}
