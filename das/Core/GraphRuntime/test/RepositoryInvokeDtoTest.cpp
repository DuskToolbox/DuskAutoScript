#include <das/Core/GraphRuntime/RepositoryInvokeDtos.h>

#include <gtest/gtest.h>

#include <string_view>
#include <type_traits>

// Migrated from TaskScheduler RepositoryInvokeDtoTest — covers the
// repository reference wire whose home is GraphRuntime (it rides with the
// closure-driven compile chain). The invokeRepository component's snapshot
// wire retired together with that component.

namespace
{
    using namespace Das::Core::GraphRuntime::RepositoryInvoke::Dto;

    template <typename T>
    constexpr bool UsesRepositoryInvokeLowerCamelRule = std::is_same_v<
        typename yyjson::field_name_rule<T>::type,
        yyjson::snake_to_camel_transform>;

    static_assert(UsesRepositoryInvokeLowerCamelRule<RepositoryTaskRefDto>);
} // namespace

TEST(RepositoryInvokeDtoTest, RepositoryRefRoundTripUsesLowerCamelFields)
{
    RepositoryTaskRefDto ref;
    ref.entry_id = 42;
    ref.expected_revision = 7;
    ref.source_fingerprint = "source-hash";

    auto serialized = yyjson::object(ref);

    EXPECT_TRUE(serialized.contains(std::string_view("kind")));
    EXPECT_TRUE(serialized.contains(std::string_view("entryId")));
    EXPECT_TRUE(serialized.contains(std::string_view("expectedRevision")));
    EXPECT_TRUE(serialized.contains(std::string_view("sourceFingerprint")));
    EXPECT_FALSE(serialized.contains(std::string_view("entry_id")));
    EXPECT_FALSE(serialized.contains(std::string_view("expected_revision")));
    EXPECT_EQ(
        serialized[std::string_view("kind")].as_string().value(),
        std::string_view("taskRepositoryRef"));

    RepositoryTaskRefDto round_tripped;
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<RepositoryTaskRefDto>(serialized));
    EXPECT_EQ(round_tripped.entry_id, 42);
    EXPECT_EQ(round_tripped.expected_revision, 7);
    EXPECT_EQ(round_tripped.source_fingerprint, "source-hash");
}
