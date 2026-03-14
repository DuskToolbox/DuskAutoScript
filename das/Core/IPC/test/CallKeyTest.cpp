#include <gtest/gtest.h>

#include <das/Core/IPC/IpcRunLoop.h>

using namespace Das::Core::IPC;

TEST(CallKeyTest, Equality)
{
    CallKey key1{1, 100};
    CallKey key2{1, 100};
    CallKey key3{1, 101};
    CallKey key4{2, 100};

    // Same values should be equal
    EXPECT_EQ(key1, key2);

    // Different call_id should not be equal
    EXPECT_NE(key1, key3);

    // Different source_session_id should not be equal
    EXPECT_NE(key1, key4);
}

TEST(CallKeyTest, Hash)
{
    CallKey key1{1, 100};
    CallKey key2{1, 100};
    CallKey key3{1, 101};

    CallKeyHash hasher;

    // Same values should have same hash
    EXPECT_EQ(hasher(key1), hasher(key2));

    // Different values usually have different hashes (but not guaranteed)
    // We just verify it compiles and runs
    size_t hash1 = hasher(key1);
    size_t hash3 = hasher(key3);
    (void)hash1;
    (void)hash3;
}

TEST(CallKeyTest, UnorderedMapUsage)
{
    std::unordered_map<CallKey, int, CallKeyHash> map;

    CallKey key1{1, 100};
    CallKey key2{1, 101};
    CallKey key3{2, 100};

    // Insert
    map[key1] = 10;
    map[key2] = 20;
    map[key3] = 30;

    // Find
    EXPECT_EQ(map.find(key1)->second, 10);
    EXPECT_EQ(map.find(key2)->second, 20);
    EXPECT_EQ(map.find(key3)->second, 30);

    // Erase
    map.erase(key2);
    EXPECT_EQ(map.find(key2), map.end());
    EXPECT_EQ(map.size(), 2);
}

TEST(CallKeyTest, DifferentSessions)
{
    // This is the key test for cross-process forwarding
    // Different sessions with same call_id should be different keys

    CallKey key_host_a{1, 100}; // Host A's call with call_id=100
    CallKey key_host_b{2, 100}; // Host B's call with call_id=100

    // They should NOT be equal (different source_session_id)
    EXPECT_NE(key_host_a, key_host_b);

    // They should have different hashes
    CallKeyHash hasher;
    EXPECT_NE(hasher(key_host_a), hasher(key_host_b));

    // Store in map - both should be able to coexist
    std::unordered_map<CallKey, std::string, CallKeyHash> pending_calls;
    pending_calls[key_host_a] = "Response from Host A";
    pending_calls[key_host_b] = "Response from Host B";

    EXPECT_EQ(pending_calls.size(), 2);
    EXPECT_EQ(pending_calls[key_host_a], "Response from Host A");
    EXPECT_EQ(pending_calls[key_host_b], "Response from Host B");
}
