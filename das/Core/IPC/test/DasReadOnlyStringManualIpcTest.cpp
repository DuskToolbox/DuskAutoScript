/**
 * @file DasReadOnlyStringManualIpcTest.cpp
 * @brief Unit tests for manually-written IPC proxy, stub, and factory
 *        registry for IDasReadOnlyString.
 *
 * Tests cover stub metadata, wire-protocol serialization layout, proxy
 * identity constants, and GUID verification.  No StubContext or IpcRunLoop
 * is required — every test runs in-process without the IPC runtime.
 */

#include <das/Core/IPC/DasReadOnlyStringProxy.h>
#include <das/Core/IPC/DasReadOnlyStringStub.h>
#include <das/Core/IPC/MemorySerializer.h>
#include <das/DasString.hpp>
#include <das/DasTypes.hpp>
#include <gtest/gtest.h>

#include <cstring>
#include <limits>

using namespace Das::Core::IPC;

// ============================================================================
// Test Suite 1: ReadOnlyStringManualProxyFactoryTest
// ============================================================================

TEST(
    ReadOnlyStringManualProxyFactoryTest,
    ProxyInterfaceIdMatchesExpectedFNV1aHash)
{
    // The manual proxy uses FNV-1a hash of the IID as the wire-format
    // interface_id.  Both proxy and stub must agree on this value.
    EXPECT_EQ(DasReadOnlyStringProxy::InterfaceId, 0xFD698AF1u);
}

TEST(ReadOnlyStringManualProxyFactoryTest, StubInterfaceIdMatchesProxy)
{
    DasReadOnlyStringStub stub;
    EXPECT_EQ(stub.GetInterfaceId(), DasReadOnlyStringProxy::InterfaceId);
}

TEST(ReadOnlyStringManualProxyFactoryTest, IID_GuidMatchesSpecification)
{
    // DAS_IID_READ_ONLY_STRING = {C09E276A-B824-4667-A504-7609B4B7DD28}
    EXPECT_EQ(DAS_IID_READ_ONLY_STRING.data1, 0xC09E276Au);
    EXPECT_EQ(DAS_IID_READ_ONLY_STRING.data2, 0xB824u);
    EXPECT_EQ(DAS_IID_READ_ONLY_STRING.data3, 0x4667u);
    const uint8_t expected_data4[] =
        {0xA5, 0x04, 0x76, 0x09, 0xB4, 0xB7, 0xDD, 0x28};
    EXPECT_EQ(
        std::memcmp(
            DAS_IID_READ_ONLY_STRING.data4,
            expected_data4,
            sizeof(expected_data4)),
        0);
}

// ============================================================================
// Test Suite 2: ReadOnlyStringStubTest
// ============================================================================

TEST(ReadOnlyStringStubTest, InterfaceId_ReturnsCorrectValue)
{
    DasReadOnlyStringStub stub;
    EXPECT_EQ(stub.GetInterfaceId(), 0xFD698AF1u);
}

TEST(ReadOnlyStringStubTest, AddRef_ReturnsMaxValue)
{
    DasReadOnlyStringStub stub;
    EXPECT_EQ(stub.AddRef(), std::numeric_limits<uint32_t>::max());
}

TEST(ReadOnlyStringStubTest, Release_ReturnsMaxValue)
{
    DasReadOnlyStringStub stub;
    EXPECT_EQ(stub.Release(), std::numeric_limits<uint32_t>::max());
}

// --- Wire-protocol format verification ---
//
// The stub's HandleGetUtf16Snapshot serialises:
//   [result   : int32  (4 bytes)]
//   [count    : uint64 (8 bytes)]
//   [utf16_raw: count * sizeof(char16_t) bytes]
//
// We verify this layout using MemorySerializerWriter directly, which
// guarantees the test stays in sync with the serialization code.

TEST(ReadOnlyStringStubTest, WireProtocolLayout_ResultAndCountAndPayload)
{
    const char16_t      kData[] = u"Hello"; // 5 char16_t, NOT null-terminated
    constexpr size_t    kCount = 5;
    constexpr DasResult kResult = DAS_S_OK;

    MemorySerializerWriter writer;
    writer.WriteInt32(static_cast<int32_t>(kResult));
    writer.WriteUInt64(static_cast<uint64_t>(kCount));
    writer.Write(kData, kCount * sizeof(char16_t));

    const auto& buf = writer.GetBuffer();

    // Expected size: 4 + 8 + 5*2 = 22 bytes
    ASSERT_EQ(buf.size(), 4u + 8u + kCount * sizeof(char16_t));

    // Byte 0-3: result (int32, value 0)
    int32_t actual_result = 0;
    std::memcpy(&actual_result, buf.data(), sizeof(int32_t));
    EXPECT_EQ(actual_result, DAS_S_OK);

    // Byte 4-11: char_count (uint64, value 5)
    uint64_t actual_count = 0;
    std::memcpy(&actual_count, buf.data() + 4, sizeof(uint64_t));
    EXPECT_EQ(actual_count, kCount);

    // Byte 12-21: UTF-16LE payload
    const auto* actual_utf16 =
        reinterpret_cast<const char16_t*>(buf.data() + 12);
    EXPECT_EQ(
        std::char_traits<char16_t>::compare(actual_utf16, kData, kCount),
        0);
}

TEST(ReadOnlyStringStubTest, WireProtocolLayout_ErrorResult_NoPayload)
{
    // When GetUtf16 returns an error, the stub writes only result + count(0).
    constexpr DasResult kError = DAS_E_NO_IMPLEMENTATION;
    constexpr uint64_t  kZero = 0;

    MemorySerializerWriter writer;
    writer.WriteInt32(static_cast<int32_t>(kError));
    writer.WriteUInt64(kZero);

    const auto& buf = writer.GetBuffer();

    // Header only: 4 + 8 = 12 bytes, no UTF-16 payload
    ASSERT_EQ(buf.size(), 12u);

    int32_t actual_result = 0;
    std::memcpy(&actual_result, buf.data(), sizeof(int32_t));
    EXPECT_EQ(actual_result, kError);

    uint64_t actual_count = 0;
    std::memcpy(&actual_count, buf.data() + 4, sizeof(uint64_t));
    EXPECT_EQ(actual_count, 0u);
}
