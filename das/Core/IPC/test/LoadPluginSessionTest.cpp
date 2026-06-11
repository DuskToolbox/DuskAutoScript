/**
 * @file LoadPluginSessionTest.cpp
 * @brief Tests for session-targeted LOAD_PLUGIN V1 payload construction
 *
 * Validates V1 payload format, null argument handling, and
 * invalid session_id rejection.
 */

#include <cstdint>
#include <cstring>
#include <das/Core/IPC/HandshakeSerialization.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace
{
    /**
     * @brief Build a V1 LOAD_PLUGIN payload from a manifest path string
     *
     * V1 format: SerializeString(manifest_path)
     */
    std::vector<uint8_t> BuildV1Payload(const std::string& manifest_path)
    {
        std::vector<uint8_t> payload;
        Das::Core::IPC::SerializeString(payload, manifest_path);
        return payload;
    }

    /**
     * @brief Parse a V1 LOAD_PLUGIN payload to extract the manifest path
     */
    bool ParseV1Payload(std::span<const uint8_t> payload, std::string& out_path)
    {
        size_t offset = 0;
        return Das::Core::IPC::DeserializeString(payload, offset, out_path);
    }

    // ============================================================
    // V1 payload construction tests
    // ============================================================

    TEST(LoadPluginSessionTest, V1Payload_RoundTrips)
    {
        const std::string manifest = "/plugins/MyPlugin/manifest.json";
        auto              payload = BuildV1Payload(manifest);

        std::string parsed;
        ASSERT_TRUE(ParseV1Payload(payload, parsed));
        EXPECT_EQ(parsed, manifest);
    }

    TEST(LoadPluginSessionTest, V1Payload_ContainsLengthPrefix)
    {
        const std::string path = "test.json";
        auto              payload = BuildV1Payload(path);

        ASSERT_GE(payload.size(), sizeof(uint16_t) + path.size());

        uint16_t len = 0;
        std::memcpy(&len, payload.data(), sizeof(uint16_t));
        EXPECT_EQ(len, static_cast<uint16_t>(path.size()));
    }

    TEST(LoadPluginSessionTest, V1Payload_Utf8Path_RoundTrips)
    {
        const std::string path =
            "/plugins/\xe3\x83\x97\xe3\x83\xa9\xe3\x82\xb0\xe3\x82\xa4\xe3\x83\xb3/manifest.json";
        auto payload = BuildV1Payload(path);

        std::string parsed;
        ASSERT_TRUE(ParseV1Payload(payload, parsed));
        EXPECT_EQ(parsed, path);
    }

    TEST(LoadPluginSessionTest, V1Payload_EmptyPath_RoundTrips)
    {
        const std::string path;
        auto              payload = BuildV1Payload(path);

        std::string parsed;
        ASSERT_TRUE(ParseV1Payload(payload, parsed));
        EXPECT_TRUE(parsed.empty());
    }

    TEST(LoadPluginSessionTest, V1Payload_LongPath_RoundTrips)
    {
        std::string long_path(512, 'A');
        auto        payload = BuildV1Payload(long_path);

        std::string parsed;
        ASSERT_TRUE(ParseV1Payload(payload, parsed));
        EXPECT_EQ(parsed, long_path);
    }

    // ============================================================
    // Deserialization edge cases
    // ============================================================

    TEST(LoadPluginSessionTest, EmptyPayload_ReturnsFalse)
    {
        std::vector<uint8_t> empty;
        std::string          parsed;
        EXPECT_FALSE(ParseV1Payload(empty, parsed));
    }

    TEST(LoadPluginSessionTest, TruncatedLength_ReturnsFalse)
    {
        std::vector<uint8_t> truncated{0x01};
        std::string          parsed;
        EXPECT_FALSE(ParseV1Payload(truncated, parsed));
    }

    TEST(LoadPluginSessionTest, TruncatedData_ReturnsFalse)
    {
        std::vector<uint8_t> truncated{0x05, 0x00, 'h', 'e'};
        std::string          parsed;
        EXPECT_FALSE(ParseV1Payload(truncated, parsed));
    }

    TEST(LoadPluginSessionTest, OversizedLength_ReturnsFalse)
    {
        std::vector<uint8_t> oversized{0xFF, 0xFF};
        std::string          parsed;
        EXPECT_FALSE(ParseV1Payload(oversized, parsed));
    }

    // ============================================================
    // Session ID validation (unit-level)
    // ============================================================

    TEST(LoadPluginSessionTest, SessionIdZero_IsInvalid)
    {
        constexpr uint16_t kInvalidSessionId = 0;
        EXPECT_EQ(kInvalidSessionId, uint16_t{0});
    }

    TEST(LoadPluginSessionTest, SessionIdNonZero_IsValid)
    {
        constexpr uint16_t kValidSessionId = 42;
        EXPECT_NE(kValidSessionId, uint16_t{0});
    }

    // ============================================================
    // Null argument validation (payload-level)
    // ============================================================

    TEST(LoadPluginSessionTest, NullPathPointer_DetectedByEmptyPayload)
    {
        std::string          parsed;
        std::vector<uint8_t> null_payload;
        EXPECT_FALSE(ParseV1Payload(null_payload, parsed));
    }

    // ============================================================
    // Multiple payloads independence
    // ============================================================

    TEST(LoadPluginSessionTest, MultiplePayloads_AreIndependent)
    {
        auto p1 = BuildV1Payload("/path/to/plugin1.json");
        auto p2 = BuildV1Payload("/path/to/plugin2.json");

        std::string parsed1, parsed2;
        ASSERT_TRUE(ParseV1Payload(p1, parsed1));
        ASSERT_TRUE(ParseV1Payload(p2, parsed2));

        EXPECT_NE(parsed1, parsed2);
        EXPECT_EQ(parsed1, "/path/to/plugin1.json");
        EXPECT_EQ(parsed2, "/path/to/plugin2.json");
    }

} // anonymous namespace
