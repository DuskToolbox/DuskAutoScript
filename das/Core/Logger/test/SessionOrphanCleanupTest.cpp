#include <das/Core/Logger/DasLogNotifierSink.h>
#include <das/Core/Logger/DasLogSessionImpl.h>
#include <das/Core/Logger/DasProcessLogRecorderImpl.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef DAS_WINDOWS
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
    std::filesystem::path UniqueTempDir(const char* test_name)
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        auto path = std::filesystem::current_path()
                    / DAS_PATH_LITERAL("logger-test-output")
                    / Das::Utils::PathFromUtf8(
                        std::string{test_name} + "-" + std::to_string(stamp));
        std::filesystem::remove_all(path);
        return path;
    }

    // 拉一个短暂退出的子进程，返回其 pid（保证已死）。
    uint64_t SpawnShortLivedChild()
    {
#ifdef DAS_WINDOWS
        STARTUPINFOA        si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        char command[] = "cmd.exe /c exit";
        if (!CreateProcessA(
                nullptr,
                command,
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &si,
                &pi))
        {
            return 0;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        const auto pid = static_cast<uint64_t>(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pid;
#else
        const pid_t pid = fork();
        if (pid == 0)
        {
            _exit(0);
        }
        if (pid > 0)
        {
            int status = 0;
            waitpid(pid, &status, 0);
        }
        return pid > 0 ? static_cast<uint64_t>(pid) : 0;
#endif
    }

    uint64_t CurrentTestPid()
    {
#ifdef DAS_WINDOWS
        return static_cast<uint64_t>(::GetCurrentProcessId());
#else
        return static_cast<uint64_t>(::getpid());
#endif
    }

    std::filesystem::path ReadUserJsonlPath(
        Das::ExportInterface::IDasLogSession* session)
    {
        Das::DasPtr<IDasReadOnlyString> path_str;
        if (DAS::IsFailed(session->GetUserJsonlPath(path_str.Put()))
            || !path_str)
        {
            return {};
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(path_str->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return {};
        }
        return std::filesystem::path{reinterpret_cast<const char8_t*>(utf8)};
    }

    std::filesystem::path ReadSessionDirectory(
        Das::ExportInterface::IDasLogSession* session)
    {
        Das::DasPtr<IDasReadOnlyString> directory_str;
        if (DAS::IsFailed(session->GetSessionDir(directory_str.Put()))
            || !directory_str)
        {
            return {};
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(directory_str->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return {};
        }
        return std::filesystem::path{reinterpret_cast<const char8_t*>(utf8)};
    }

    class SessionOrphanFixture : public ::testing::Test
    {
    protected:
        Das::DasPtr<Das::ExportInterface::IDasLogNotifier>        notifier_;
        Das::DasPtr<Das::ExportInterface::IDasLogSession>         session_;
        Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder_;

        void SetUp() override { GetDasLogNotifier(notifier_.Put()); }

        DasResult CreateContext(const std::filesystem::path& debug_directory)
        {
            const std::string directory_str =
                Das::Utils::PathToUtf8(debug_directory);
            Das::DasPtr<IDasReadOnlyString> directory_param;
            const auto cr = CreateIDasReadOnlyStringFromUtf8(
                directory_str.c_str(),
                directory_param.Put());
            if (DAS::IsFailed(cr))
            {
                return cr;
            }
            const auto session_cr =
                CreateLogSession(directory_param.Get(), session_.Put());
            if (DAS::IsFailed(session_cr))
            {
                return session_cr;
            }
            return CreateIDasProcessLogRecorder(
                notifier_.Get(),
                session_.Get(),
                recorder_.Put());
        }
    };
} // namespace

// 基础图片能力：AddImage 的 8x8 RGB 图在 Record 时经 Core JPG 编码落盘，
// imageRefs 填相对路径，且落盘文件可解码回读同尺寸。
TEST_F(
    SessionOrphanFixture,
    SessionOrphanCleanupTest_AddImageWritesJpgWithRelativeRef)
{
    const auto dir = UniqueTempDir("AddImageJpg");
    ASSERT_EQ(CreateContext(dir), DAS_S_OK);

    auto entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
        new Das::Core::IDasLogEntryImpl{}};
    DasU8StringOnStack text{"focus"};
    ASSERT_EQ(
        entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_STRUCTURAL),
        DAS_S_OK);
    ASSERT_EQ(entry->AddText(&text), DAS_S_OK);

    constexpr int32_t width = 8;
    constexpr int32_t height = 8;
    std::vector<char> pixels(
        static_cast<size_t>(width) * height * 3,
        static_cast<char>(0x80));
    DasImageDesc desc{
        pixels.data(),
        pixels.size(),
        Das::ExportInterface::DAS_IMAGE_FORMAT_RGB_888};
    Das::ExportInterface::DasSize                size{width, height};
    Das::DasPtr<Das::ExportInterface::IDasImage> image;
    ASSERT_EQ(
        CreateIDasImageFromDecodedData(&desc, &size, image.Put()),
        DAS_S_OK);
    ASSERT_EQ(entry->AddImage(image.Get()), DAS_S_OK);

    ASSERT_EQ(recorder_->Record(entry.Get()), DAS_S_OK);

    const auto jsonl_path = ReadUserJsonlPath(session_.Get());
    ASSERT_FALSE(jsonl_path.empty());
    ASSERT_TRUE(std::filesystem::exists(jsonl_path));

    std::ifstream input{jsonl_path};
    std::string   first_line;
    ASSERT_TRUE(std::getline(input, first_line));

    auto parsed = DAS::Utils::ParseYyjsonFromString(first_line);
    ASSERT_TRUE(parsed.has_value());
    auto obj = parsed->as_object();
    ASSERT_TRUE(obj.has_value());

    auto image_refs = (*obj)[std::string_view("imageRefs")].as_array();
    ASSERT_TRUE(image_refs.has_value());
    ASSERT_EQ(image_refs->size(), 1u);
    auto ref_value = (*image_refs)[0].as_string();
    ASSERT_TRUE(ref_value.has_value());
    EXPECT_EQ(*ref_value, "img/0001.jpg");

    const auto img_path = ReadSessionDirectory(session_.Get())
                          / DAS_PATH_LITERAL("img")
                          / DAS_PATH_LITERAL("0001.jpg");
    ASSERT_TRUE(std::filesystem::exists(img_path));

    std::ifstream     file_input{img_path, std::ios::binary};
    std::vector<char> bytes{
        std::istreambuf_iterator<char>{file_input},
        std::istreambuf_iterator<char>{}};
    ASSERT_FALSE(bytes.empty());

    DasImageDesc jpg_desc{
        bytes.data(),
        bytes.size(),
        Das::ExportInterface::DAS_IMAGE_FORMAT_JPG};
    Das::DasPtr<Das::ExportInterface::IDasImage> decoded;
    ASSERT_EQ(
        CreateIDasImageFromEncodedData(&jpg_desc, decoded.Put()),
        DAS_S_OK);
    Das::ExportInterface::DasSize decoded_size{};
    ASSERT_EQ(decoded->GetSize(&decoded_size), DAS_S_OK);
    EXPECT_EQ(decoded_size.width, width);
    EXPECT_EQ(decoded_size.height, height);
}

// 启动清理孤儿会话目录——死 pid 目录被删除、活 pid 目录保留、
// 非会话目录不动、本次新会话目录已创建。清理逻辑在 CreateLogSession ctor 中。
TEST_F(
    SessionOrphanFixture,
    SessionOrphanCleanupTest_RemovesDeadPidKeepsLivePid)
{
    const auto dir = UniqueTempDir("OrphanCleanup");
    const auto debug_dir = dir / DAS_PATH_LITERAL("debug");
    std::filesystem::create_directories(debug_dir);

    const auto dead_pid = SpawnShortLivedChild();
    ASSERT_GT(dead_pid, 0u);
    const auto live_pid = CurrentTestPid();

    const auto dead_dir =
        debug_dir
        / Das::Utils::PathFromUtf8(
            std::string{"20260801-120000-"} + std::to_string(dead_pid));
    const auto live_dir =
        debug_dir
        / Das::Utils::PathFromUtf8(
            std::string{"20260801-120000-"} + std::to_string(live_pid));
    const auto other_dir = debug_dir / DAS_PATH_LITERAL("something-else");
    std::filesystem::create_directories(dead_dir);
    std::filesystem::create_directories(live_dir);
    std::filesystem::create_directories(other_dir);

    ASSERT_EQ(CreateContext(debug_dir), DAS_S_OK);

    EXPECT_FALSE(std::filesystem::exists(dead_dir));
    EXPECT_TRUE(std::filesystem::exists(live_dir));
    EXPECT_TRUE(std::filesystem::exists(other_dir));
    EXPECT_FALSE(ReadSessionDirectory(session_.Get()).empty());
}
