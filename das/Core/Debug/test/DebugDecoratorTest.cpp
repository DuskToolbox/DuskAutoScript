#include <das/Core/Debug/DebugEvent.h>
#include <das/Core/Debug/DebugRuntime.h>
#include <das/DasConfig.h>
#include <das/DasTypes.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef DAS_WINDOWS
#include <stdlib.h>
#endif

namespace Das::Core::Debug::Test
{
    namespace
    {
        auto UniqueTempDir(const char* test_name) -> std::filesystem::path
        {
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            auto path = std::filesystem::current_path()
                / "debug-test-output"
                / (std::string{test_name} + "-" + std::to_string(stamp));
            std::filesystem::remove_all(path);
            return path;
        }

        void SetDasDebug(const char* value)
        {
#ifdef DAS_WINDOWS
            _putenv_s("DAS_DEBUG", value);
#else
            setenv("DAS_DEBUG", value, 1);
#endif
        }

        void UnsetDasDebug()
        {
#ifdef DAS_WINDOWS
            _putenv_s("DAS_DEBUG", "");
#else
            unsetenv("DAS_DEBUG");
#endif
        }

        void ResetRuntime()
        {
            DebugRuntime::Shutdown();
            DebugRuntime::ResetForTest();
            UnsetDasDebug();
        }

        class DebugRuntimeFixture : public ::testing::Test
        {
        protected:
            void TearDown() override { ResetRuntime(); }
        };

    } // namespace

    TEST_F(DebugRuntimeFixture, DebugRuntimeTest_DasDebugOneEnables)
    {
        const auto dir = UniqueTempDir("DasDebugOneEnables");
        SetDasDebug("1");

        DebugRuntimeOptions options{};
        options.debug_dir = dir;
        ASSERT_EQ(DebugRuntime::Initialize(options), DAS_S_OK);

        EXPECT_TRUE(DebugRuntime::IsEnabled());
        EXPECT_EQ(DebugRuntime::DebugDir(), std::filesystem::absolute(dir));
    }

    TEST_F(DebugRuntimeFixture, DebugRuntimeTest_DasDebugZeroDisables)
    {
        const auto dir = UniqueTempDir("DasDebugZeroDisables");
        SetDasDebug("0");

        DebugRuntimeOptions options{};
        options.debug_dir = dir;
        ASSERT_EQ(DebugRuntime::Initialize(options), DAS_S_OK);

        EXPECT_FALSE(DebugRuntime::IsEnabled());
        EXPECT_EQ(DebugRuntime::DebugDir(), std::filesystem::absolute(dir));
    }

    TEST_F(DebugRuntimeFixture, DebugRuntimeTest_DefaultDependsOnBuildType)
    {
        const auto dir = UniqueTempDir("DefaultDependsOnBuildType");
        UnsetDasDebug();

        DebugRuntimeOptions options{};
        options.debug_dir = dir;
        ASSERT_EQ(DebugRuntime::Initialize(options), DAS_S_OK);

#ifdef _DEBUG
        EXPECT_TRUE(DebugRuntime::IsEnabled());
#else
        EXPECT_FALSE(DebugRuntime::IsEnabled());
#endif
    }

    TEST_F(
        DebugRuntimeFixture,
        DebugRuntimeTest_InitOptionsDebugDirFallbackAndOverride)
    {
        UnsetDasDebug();

        DebugRuntimeOptions fallback_options{};
        ASSERT_EQ(DebugRuntime::Initialize(fallback_options), DAS_S_OK);
        EXPECT_EQ(
            DebugRuntime::DebugDir(),
            std::filesystem::absolute("logs/debug"));

        DebugRuntime::Shutdown();
        DebugRuntime::ResetForTest();

        const auto override_dir = UniqueTempDir("DebugDirOverride");
        DebugRuntimeOptions override_options{};
        override_options.debug_dir = override_dir;
        ASSERT_EQ(DebugRuntime::Initialize(override_options), DAS_S_OK);
        EXPECT_EQ(
            DebugRuntime::DebugDir(),
            std::filesystem::absolute(override_dir));
    }

    TEST_F(
        DebugRuntimeFixture,
        DebugDisabledNoopTest_DisabledRuntimeCreatesNoWriterFiles)
    {
        const auto dir = UniqueTempDir("DisabledRuntimeCreatesNoWriterFiles");
        SetDasDebug("0");

        DebugRuntimeOptions options{};
        options.debug_dir = dir;
        ASSERT_EQ(DebugRuntime::Initialize(options), DAS_S_OK);

        DebugEvent event{};
        event.type = "noop";
        event.params_json = "{}";
        event.result_json = "{}";
        ASSERT_EQ(DebugRuntime::SubmitEvent(event), DAS_S_OK);
        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);

        EXPECT_FALSE(std::filesystem::exists(dir / "debug.jsonl"));
        EXPECT_FALSE(std::filesystem::exists(dir / "img"));
    }

} // namespace Das::Core::Debug::Test
