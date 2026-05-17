#include <das/Core/Debug/DebugEvent.h>
#include <das/Core/Debug/DebugImageAnnotator.h>
#include <das/Core/Debug/DebugRuntime.h>
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/OcvWrapper/CpuImageImpl.hpp>
#include <das/DasApi.h>
#include <das/DasConfig.h>
#include <das/DasPtr.hpp>
#include <das/DasTypes.hpp>
#include <gtest/gtest.h>

#include "../src/DebugWriterImpl.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
            auto path =
                std::filesystem::current_path() / "debug-test-output"
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

        auto ReadLines(const std::filesystem::path& path)
            -> std::vector<std::string>
        {
            std::ifstream            input{path};
            std::vector<std::string> lines;
            std::string              line;
            while (std::getline(input, line))
            {
                lines.emplace_back(std::move(line));
            }
            return lines;
        }

        void ExpectContains(
            const std::string& haystack,
            const std::string& needle)
        {
            EXPECT_NE(haystack.find(needle), std::string::npos) << haystack;
        }

        auto MakeSnapshot() -> std::shared_ptr<DebugImageSnapshot>
        {
            cv::Mat mat = cv::Mat::zeros(32, 48, CV_8UC3);
            mat.setTo(cv::Scalar{32, 64, 128});
            auto* image =
                OcvWrapper::CpuImageImpl<OcvWrapper::Storage::OwningStorage>::
                    MakeFromCpuMat(mat, ExportInterface::DAS_PIXEL_FORMAT_BGR);
            DasPtr<ExportInterface::IDasImage> image_guard(image);
            return CaptureImageSnapshot(image);
        }

        void InitializeEnabledRuntime(const std::filesystem::path& dir)
        {
            SetDasDebug("1");
            DebugRuntimeOptions options{};
            options.debug_dir = dir;
            ASSERT_EQ(DebugRuntime::Initialize(options), DAS_S_OK);
        }

        auto RegisterWriterForRuntime(const std::filesystem::path& dir)
            -> DAS::Core::IPC::MainProcess::IpcContextPtr
        {
            InitializeEnabledRuntime(dir);
            auto ctx = DAS::Core::IPC::MainProcess::CreateIpcContextEz(false);
            EXPECT_NE(ctx.get(), nullptr);
            DAS::Core::IPC::ScopedCurrentIpcContext scope(
                static_cast<DAS::Core::IPC::MainProcess::IpcContext*>(
                    ctx.get()));
            EXPECT_EQ(RegisterDebugWriterService(*ctx), DAS_S_OK);
            return ctx;
        }

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

        const auto          override_dir = UniqueTempDir("DebugDirOverride");
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

    TEST_F(
        DebugRuntimeFixture,
        DebugWriterJsonlTest_AssignsMonotonicStepsAndRequiredSchema)
    {
        const auto dir = UniqueTempDir("WriterJsonlSchema");
        auto*      writer = DebugWriterImpl::MakeRaw(dir);

        DasU8StringOnStack first{
            "{\"type\":\"first\",\"timestamp\":\"2026-05-07T01:02:03Z\","
            "\"params\":{\"a\":1},\"result\":{\"ok\":true},"
            "\"elapsed_ms\":12.5,\"image_filename\":\"img.png\"}"};
        DasU8StringOnStack second{
            "{\"type\":\"second\",\"params\":{},\"result\":{}}"};

        ASSERT_EQ(writer->LogEntry(&first), DAS_S_OK);
        ASSERT_EQ(writer->LogEntry(&second), DAS_S_OK);
        ASSERT_EQ(writer->Flush(), DAS_S_OK);
        writer->Shutdown();
        writer->Release();

        const auto lines = ReadLines(dir / "debug.jsonl");
        ASSERT_EQ(lines.size(), 2U);
        ExpectContains(lines[0], "\"step\":1");
        ExpectContains(lines[1], "\"step\":2");
        ExpectContains(lines[0], "\"type\":\"first\"");
        ExpectContains(lines[0], "\"timestamp\":\"2026-05-07T01:02:03Z\"");
        ExpectContains(lines[0], "\"params\":{\"a\":1}");
        ExpectContains(lines[0], "\"result\":{\"ok\":true}");
        ExpectContains(lines[0], "\"elapsed_ms\":12.5");
        ExpectContains(lines[0], "\"image_filename\":\"img.png\"");
        ExpectContains(lines[0], "\"timestamp\"");
        ExpectContains(lines[0], "\"params\"");
        ExpectContains(lines[0], "\"result\"");
        ExpectContains(lines[0], "\"elapsed_ms\"");
        ExpectContains(lines[0], "\"thread_id\"");
        ExpectContains(lines[0], "\"process_pid\"");
        ExpectContains(lines[0], "\"image_filename\"");
    }

    TEST_F(
        DebugRuntimeFixture,
        DebugWriterJsonlTest_HostSenderUsesDebugWriterByName)
    {
        const auto dir = UniqueTempDir("WriterByName");
        auto       ctx = RegisterWriterForRuntime(dir);

        DAS::Core::IPC::ScopedCurrentIpcContext scope(
            static_cast<DAS::Core::IPC::MainProcess::IpcContext*>(ctx.get()));
        IDasBase* p_base = nullptr;
        ASSERT_EQ(
            DasQueryMainProcessInterfaceByName("debug.writer", &p_base),
            DAS_S_OK);
        DasPtr<IDasBase> base_guard = DasPtr<IDasBase>::Attach(p_base);

        DasPtr<ExportInterface::IDasDebugWriter> writer;
        ASSERT_EQ(base_guard.As(writer), DAS_S_OK);
        ASSERT_NE(writer.Get(), nullptr);

        ctx->UnregisterServiceByName("debug.writer");
    }

    TEST_F(DebugRuntimeFixture, DebugWriterJsonlTest_FlushWritesQueuedMetadata)
    {
        const auto dir = UniqueTempDir("FlushWritesMetadata");
        auto       ctx = RegisterWriterForRuntime(dir);

        auto event = MakeDebugEvent("flush_metadata", "{}", "{}");
        ASSERT_EQ(DebugRuntime::SubmitEvent(event), DAS_S_OK);
        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);

        const auto lines = ReadLines(dir / "debug.jsonl");
        ASSERT_EQ(lines.size(), 1U);
        ExpectContains(lines[0], "\"type\":\"flush_metadata\"");

        ctx->UnregisterServiceByName("debug.writer");
    }

    TEST_F(
        DebugRuntimeFixture,
        DebugRuntimeFlushTest_DrainsMetadataAndImageJobsBeforeAssertions)
    {
        const auto dir = UniqueTempDir("FlushDrainsImageJobs");
        auto       ctx = RegisterWriterForRuntime(dir);
        auto       snapshot = MakeSnapshot();
        DebugRuntime::SetLatestImage(snapshot);

        DebugDrawBox box{};
        box.rect = {2, 2, 10, 12};
        box.label = "score=1.0";
        const auto image_result =
            SaveOriginalAndAnnotated("runtime_flush", snapshot, {box});

        auto event =
            MakeDebugEvent("runtime_flush", "{}", BuildImageJson(image_result));
        event.image_filename = image_result.image_filename;
        ASSERT_EQ(DebugRuntime::SubmitEvent(event), DAS_S_OK);
        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);

        EXPECT_TRUE(std::filesystem::exists(dir / "debug.jsonl"));
        EXPECT_TRUE(
            std::filesystem::exists(dir / "img" / image_result.image_filename));
        EXPECT_TRUE(
            std::filesystem::exists(
                dir / "img" / image_result.original_image_filename));

        ctx->UnregisterServiceByName("debug.writer");
    }

    TEST_F(
        DebugRuntimeFixture,
        DebugImageWorkerTest_FilenamesIgnoreTextAndParams)
    {
        const auto dir = UniqueTempDir("FilenameSanitization");
        InitializeEnabledRuntime(dir);
        auto snapshot = MakeSnapshot();

        const auto image_result = SaveOriginalAndAnnotated(
            "ocr_result",
            snapshot,
            std::vector<DebugDrawBox>{});
        ASSERT_EQ(image_result.image_status, "available");
        EXPECT_EQ(
            image_result.image_filename.find("hello_secret_text"),
            std::string::npos);
        EXPECT_EQ(
            image_result.original_image_filename.find("hello_secret_text"),
            std::string::npos);
        EXPECT_NE(
            image_result.image_filename.find("annotated_"),
            std::string::npos);

        ASSERT_EQ(DrainImageJobs(), DAS_S_OK);
    }

    TEST_F(
        DebugRuntimeFixture,
        DebugImageWorkerTest_UnsupportedImageWritesNotAvailableStatus)
    {
        const auto snapshot = CaptureImageSnapshot(nullptr);
        const auto image_result = SaveOriginalAndAnnotated(
            "unsupported",
            snapshot,
            std::vector<DebugDrawBox>{});
        EXPECT_EQ(image_result.image_status, "not_available");
        ExpectContains(BuildImageJson(image_result), "\"not_available\"");
    }

    TEST_F(
        DebugRuntimeFixture,
        DebugRuntimeShutdownTest_ShutdownDrainsAndClearsLatestImage)
    {
        const auto dir = UniqueTempDir("ShutdownDrain");
        auto       ctx = RegisterWriterForRuntime(dir);
        auto       snapshot = MakeSnapshot();
        DebugRuntime::SetLatestImage(snapshot);

        const auto image_result = SaveOriginalAndAnnotated(
            "shutdown_drain",
            snapshot,
            std::vector<DebugDrawBox>{});
        auto event = MakeDebugEvent(
            "shutdown_drain",
            "{}",
            BuildImageJson(image_result));
        event.image_filename = image_result.image_filename;
        ASSERT_EQ(DebugRuntime::SubmitEvent(event), DAS_S_OK);

        DebugRuntime::Shutdown();

        EXPECT_TRUE(std::filesystem::exists(dir / "debug.jsonl"));
        EXPECT_TRUE(
            std::filesystem::exists(dir / "img" / image_result.image_filename));
        EXPECT_EQ(DebugRuntime::GetLatestImage(), nullptr);

        ctx->UnregisterServiceByName("debug.writer");
    }

    TEST_F(
        DebugRuntimeFixture,
        DebugDisabledNoopTest_DisabledRuntimeCreatesNoWorkerThreads)
    {
        const auto dir = UniqueTempDir("DisabledRuntimeNoWorkers");
        SetDasDebug("0");

        DebugRuntimeOptions options{};
        options.debug_dir = dir;
        ASSERT_EQ(DebugRuntime::Initialize(options), DAS_S_OK);
        ASSERT_EQ(DebugRuntime::Flush(), DAS_S_OK);

        EXPECT_FALSE(IsImageWorkerRunningForTest());
        EXPECT_FALSE(std::filesystem::exists(dir / "debug.jsonl"));
        EXPECT_FALSE(std::filesystem::exists(dir / "img"));
    }

} // namespace Das::Core::Debug::Test
