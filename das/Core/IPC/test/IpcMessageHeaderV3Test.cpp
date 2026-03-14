/**
 * @file IpcMessageHeaderV3Test.cpp
 * @brief V3 Header unit tests
 *
 * Tests for the V3 IPCMessageHeader structure:
 * - 32 bytes size, 8 bytes alignment
 * - MAGIC = 0x43504944 (correct "DIPC" little-endian)
 * - version = 3
 * - Contains source_session_id, target_session_id, interface_id
 */

#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <gtest/gtest.h>
#include <cstring>

namespace Das::Core::IPC::Test
{

// ==================== Size and Alignment Tests ====================

TEST(IpcMessageHeaderV3Test, SizeAndAlignment)
{
    // V3 Header should be 32 bytes
    EXPECT_EQ(sizeof(IPCMessageHeader), 32);
    // V3 Header should be 8-byte aligned
    EXPECT_EQ(alignof(IPCMessageHeader), 8);
}

TEST(IpcMessageHeaderV3Test, MagicValue)
{
    // MAGIC should be 0x43504944 (correct "DIPC" little-endian)
    EXPECT_EQ(IPCMessageHeader::MAGIC, 0x43504944u);

    // Verify the magic bytes spell "DIPC" in memory (little-endian)
    IPCMessageHeader header = {};
    header.magic = IPCMessageHeader::MAGIC;

    // In little-endian: 0x43504944 = 'D'(0x44) 'I'(0x49) 'P'(0x50) 'C'(0x43)
    const char* magic_bytes = reinterpret_cast<const char*>(&header.magic);
    EXPECT_EQ(magic_bytes[0], 'D'); // 0x44
    EXPECT_EQ(magic_bytes[1], 'I'); // 0x49
    EXPECT_EQ(magic_bytes[2], 'P'); // 0x50
    EXPECT_EQ(magic_bytes[3], 'C'); // 0x43
}

TEST(IpcMessageHeaderV3Test, VersionValue)
{
    // V3 Header version should be 3
    EXPECT_EQ(IPCMessageHeader::CURRENT_VERSION, 3);
}

// ==================== IsValid Tests ====================

TEST(IpcMessageHeaderV3Test, IsValid)
{
    // Valid header: correct magic and version
    IPCMessageHeader valid_header = {};
    valid_header.magic = IPCMessageHeader::MAGIC;
    valid_header.version = IPCMessageHeader::CURRENT_VERSION;
    EXPECT_TRUE(valid_header.IsValid());

    // Invalid: wrong magic
    IPCMessageHeader invalid_magic = {};
    invalid_magic.magic = 0x12345678;
    invalid_magic.version = IPCMessageHeader::CURRENT_VERSION;
    EXPECT_FALSE(invalid_magic.IsValid());

    // Invalid: wrong version
    IPCMessageHeader invalid_version = {};
    invalid_version.magic = IPCMessageHeader::MAGIC;
    invalid_version.version = 2; // V2
    EXPECT_FALSE(invalid_version.IsValid());

    // Invalid: both wrong
    IPCMessageHeader invalid_both = {};
    invalid_both.magic = 0x12345678;
    invalid_both.version = 99;
    EXPECT_FALSE(invalid_both.IsValid());
}

// ==================== Field Layout Tests ====================

TEST(IpcMessageHeaderV3Test, FieldLayout)
{
    // Verify field offsets for V3 Header (32 bytes, alignas(8))
    // Layout:
    // - magic: offset 0 (4 bytes)
    // - version: offset 4 (2 bytes)
    // - message_type: offset 6 (1 byte)
    // - header_flags: offset 7 (1 byte)
    // - call_id: offset 8 (2 bytes)
    // - source_session_id: offset 10 (2 bytes)
    // - target_session_id: offset 12 (2 bytes)
    // - flags: offset 14 (2 bytes)
    // - interface_id: offset 16 (4 bytes)
    // - error_code: offset 20 (4 bytes)
    // - body_size: offset 24 (4 bytes)
    // - reserved: offset 28 (4 bytes)

    EXPECT_EQ(offsetof(IPCMessageHeader, magic), 0);
    EXPECT_EQ(offsetof(IPCMessageHeader, version), 4);
    EXPECT_EQ(offsetof(IPCMessageHeader, message_type), 6);
    EXPECT_EQ(offsetof(IPCMessageHeader, header_flags), 7);
    EXPECT_EQ(offsetof(IPCMessageHeader, call_id), 8);
    EXPECT_EQ(offsetof(IPCMessageHeader, source_session_id), 10);
    EXPECT_EQ(offsetof(IPCMessageHeader, target_session_id), 12);
    EXPECT_EQ(offsetof(IPCMessageHeader, flags), 14);
    EXPECT_EQ(offsetof(IPCMessageHeader, interface_id), 16);
    EXPECT_EQ(offsetof(IPCMessageHeader, error_code), 20);
    EXPECT_EQ(offsetof(IPCMessageHeader, body_size), 24);
    EXPECT_EQ(offsetof(IPCMessageHeader, reserved), 28);
}

// ==================== Field Existence Tests ====================

TEST(IpcMessageHeaderV3Test, HasSourceSessionId)
{
    IPCMessageHeader header = {};
    header.source_session_id = 0x1234;
    EXPECT_EQ(header.source_session_id, 0x1234u);
}

TEST(IpcMessageHeaderV3Test, HasTargetSessionId)
{
    IPCMessageHeader header = {};
    header.target_session_id = 0x5678;
    EXPECT_EQ(header.target_session_id, 0x5678u);
}

TEST(IpcMessageHeaderV3Test, HasInterfaceId)
{
    IPCMessageHeader header = {};
    header.interface_id = 0xABCDEF01;
    EXPECT_EQ(header.interface_id, 0xABCDEF01u);
}

TEST(IpcMessageHeaderV3Test, HasReserved)
{
    IPCMessageHeader header = {};
    header.reserved = 0xDEADBEEF;
    EXPECT_EQ(header.reserved, 0xDEADBEEFu);
}

TEST(IpcMessageHeaderV3Test, CallIdIsUint16)
{
    // Verify call_id is uint16_t (16-bit, not 64-bit)
    IPCMessageHeader header = {};
    header.call_id = 0xFFFF;
    EXPECT_EQ(header.call_id, 0xFFFFu);

    // Verify the size of call_id field
    // This test ensures we're using uint16_t, not uint64_t
    EXPECT_EQ(sizeof(header.call_id), 2);
}

// ==================== Builder Integration Tests ====================

TEST(IpcMessageHeaderV3Test, BuilderIntegration)
{
    // Use Builder to construct a V3 header
    auto validated = IPCMessageHeaderBuilder()
        .SetMessageType(MessageType::REQUEST)
        .SetCallId(0x1234)
        .SetSourceSessionId(0x100)
        .SetTargetSessionId(0x200)
        .SetInterfaceId(0xABCDEF)
        .SetBodySize(1024)
        .Build();

    const IPCMessageHeader& header = validated.Raw();

    EXPECT_EQ(header.magic, IPCMessageHeader::MAGIC);
    EXPECT_EQ(header.version, 3);
    EXPECT_EQ(header.message_type, static_cast<uint8_t>(MessageType::REQUEST));
    EXPECT_EQ(header.call_id, 0x1234u);
    EXPECT_EQ(header.source_session_id, 0x100u);
    EXPECT_EQ(header.target_session_id, 0x200u);
    EXPECT_EQ(header.interface_id, 0xABCDEFu);
    EXPECT_EQ(header.body_size, 1024u);
    EXPECT_TRUE(header.IsValid());
}

TEST(IpcMessageHeaderV3Test, BuilderSetSourceSessionId)
{
    auto validated = IPCMessageHeaderBuilder()
        .SetSourceSessionId(0x1234)
        .Build();

    EXPECT_EQ(validated.Raw().source_session_id, 0x1234u);
}

TEST(IpcMessageHeaderV3Test, BuilderSetTargetSessionId)
{
    auto validated = IPCMessageHeaderBuilder()
        .SetTargetSessionId(0x5678)
        .Build();

    EXPECT_EQ(validated.Raw().target_session_id, 0x5678u);
}

TEST(IpcMessageHeaderV3Test, BuilderSetCallIdUint16)
{
    // SetCallId should accept uint16_t
    auto validated = IPCMessageHeaderBuilder()
        .SetCallId(0xFFFF)
        .Build();

    EXPECT_EQ(validated.Raw().call_id, 0xFFFFu);
}

TEST(IpcMessageHeaderV3Test, BuilderSetInterfaceId)
{
    // SetInterfaceId should exist for control plane messages
    auto validated = IPCMessageHeaderBuilder()
        .SetInterfaceId(0x12345678)
        .Build();

    EXPECT_EQ(validated.Raw().interface_id, 0x12345678u);
}

} // namespace Das::Core::IPC::Test
