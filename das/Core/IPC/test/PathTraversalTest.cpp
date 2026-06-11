/**
 * @file PathTraversalTest.cpp
 * @brief Tests for path validation logic in LIST_FILE / READ_FILE handlers
 *
 * Validates that the Host-side file access handlers reject path traversal
 * attempts, absolute paths, and other security-sensitive inputs while
 * accepting valid relative paths.
 */

#include <cstdint>
#include <cstring>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HandshakeSerialization.h>
#include <das/Core/IPC/Host/HostCommandHandlers.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <unordered_map>

namespace
{
    using Das::Core::IPC::IpcCommandResponse;
    using Das::Core::IPC::IpcCommandType;
    using Das::Core::IPC::ObjectId;
    using Das::Core::IPC::SerializeString;
    using Das::Core::IPC::SerializeValue;
    using Das::Core::IPC::ValidatedIPCMessageHeader;
    using Das::Core::IPC::Host::CommandHandler;
    using Das::Core::IPC::Host::HostCommandHandlerOptions;
    using Das::Core::IPC::Host::IIpcContext;
    using Das::Core::IPC::Host::RegisterHostCommandHandlers;

    class FakeHostContext final : public IIpcContext
    {
    public:
        FakeHostContext() { object_manager_.SetSessionId(1); }

        DasResult Run() override { return DAS_S_OK; }
        void      RequestStop() override {}
        bool      IsConnected() const override { return true; }

        void RegisterCommandHandler(uint32_t cmd_type, CommandHandler handler)
            override
        {
            handlers[cmd_type] = std::move(handler);
        }

        void PostCallback(IDasAsyncCallback*) override {}

        DasResult RegisterLocalObject(IDasBase* obj, ObjectId& out_id) override
        {
            return object_manager_.RegisterLocalObject(obj, out_id);
        }

        Das::Core::IPC::IDistributedObjectManager& GetObjectManager() override
        {
            return object_manager_;
        }

        DasResult ResolveMainProcessInterface(const DasGuid&, IDasBase**)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult ResolveMainProcessInterfaceByName(const char*, IDasBase**)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        std::unordered_map<uint32_t, CommandHandler> handlers;

    private:
        Das::Core::IPC::DistributedObjectManager object_manager_;
    };

    struct PathTraversalTest : public ::testing::Test
    {
        void SetUp() override
        {
            tmp_dir_ = std::filesystem::temp_directory_path()
                       / "das_path_traversal_test";
            std::filesystem::create_directories(tmp_dir_);
            std::filesystem::create_directories(tmp_dir_ / "subdir");

            HostCommandHandlerOptions opts{};
            opts.load_plugin = [](const std::filesystem::path&)
                -> Das::Utils::Expected<DAS::DasPtr<IDasBase>>
            { return DAS::DasPtr<IDasBase>(static_cast<IDasBase*>(nullptr)); };
            opts.plugin_dir = tmp_dir_;

            ASSERT_EQ(
                RegisterHostCommandHandlers(&ctx_, std::move(opts)),
                DAS_S_OK);
        }

        void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

        DasResult CallListFile(
            const std::string&  relative_path,
            IpcCommandResponse& response)
        {
            auto it = ctx_.handlers.find(
                static_cast<uint32_t>(IpcCommandType::LIST_FILE));
            if (it == ctx_.handlers.end())
            {
                return DAS_E_FAIL;
            }

            std::vector<uint8_t> payload;
            SerializeString(payload, relative_path);
            SerializeValue(payload, uint8_t{0});

            return it->second(ValidatedIPCMessageHeader{}, payload, response);
        }

        DasResult CallReadFile(
            const std::string&  relative_path,
            IpcCommandResponse& response)
        {
            auto it = ctx_.handlers.find(
                static_cast<uint32_t>(IpcCommandType::READ_FILE));
            if (it == ctx_.handlers.end())
            {
                return DAS_E_FAIL;
            }

            std::vector<uint8_t> payload;
            SerializeString(payload, relative_path);

            return it->second(ValidatedIPCMessageHeader{}, payload, response);
        }

        FakeHostContext       ctx_;
        std::filesystem::path tmp_dir_;
    };

    // ============================================================
    // ".." rejection tests
    // ============================================================

    TEST_F(PathTraversalTest, DoubleDotRejected_ListFile)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(CallListFile("..", resp), DAS_E_INVALID_PATH);
    }

    TEST_F(PathTraversalTest, DoubleDotRejected_ReadFile)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(CallReadFile("..", resp), DAS_E_INVALID_PATH);
    }

    TEST_F(PathTraversalTest, DoubleDotSlashRejected_ListFile)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(CallListFile("../etc/passwd", resp), DAS_E_INVALID_PATH);
    }

    TEST_F(PathTraversalTest, DoubleDotSlashRejected_ReadFile)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(CallReadFile("../etc/passwd", resp), DAS_E_INVALID_PATH);
    }

    TEST_F(PathTraversalTest, EmbeddedDoubleDotRejected)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(
            CallListFile("subdir/../../etc/passwd", resp),
            DAS_E_INVALID_PATH);
    }

    TEST_F(PathTraversalTest, TrailingDoubleDotRejected)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(CallListFile("subdir/..", resp), DAS_E_INVALID_PATH);
    }

    // ============================================================
    // Absolute path rejection tests
    // ============================================================

    TEST_F(PathTraversalTest, UnixAbsolutePathRejected)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(CallListFile("/etc/passwd", resp), DAS_E_INVALID_PATH);
    }

    TEST_F(PathTraversalTest, WindowsDriveAbsolutePathRejected)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(
            CallReadFile("C:\\Windows\\System32", resp),
            DAS_E_INVALID_PATH);
    }

    TEST_F(PathTraversalTest, LeadingSlashRejected)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(CallListFile("/tmp", resp), DAS_E_INVALID_PATH);
    }

    TEST_F(PathTraversalTest, LeadingBackslashRejected)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(CallListFile("\\tmp", resp), DAS_E_INVALID_PATH);
    }

    // ============================================================
    // Valid relative path acceptance tests
    // ============================================================

    TEST_F(PathTraversalTest, EmptyPathAccepted_ListFile)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(CallListFile("", resp), DAS_S_OK);
    }

    TEST_F(PathTraversalTest, ValidRelativePathAccepted_ListFile)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(CallListFile("subdir", resp), DAS_S_OK);
    }

    TEST_F(PathTraversalTest, ValidSubdirectoryAccepted_ReadFile)
    {
        auto file_path = tmp_dir_ / "subdir" / "test.txt";
        std::ofstream(file_path) << "hello";

        IpcCommandResponse resp{};
        EXPECT_EQ(CallReadFile("subdir/test.txt", resp), DAS_S_OK);
    }

    /**
     * @brief Parse entry count from a LIST_FILE response
     * Response format: SerializeString(working_path) + uint16_t entry_count
     */
    uint16_t ParseListFileEntryCount(const std::vector<uint8_t>& data)
    {
        size_t      offset = 0;
        std::string working_path;
        if (!Das::Core::IPC::DeserializeString(
                std::span<const uint8_t>(data),
                offset,
                working_path))
        {
            return 0;
        }
        uint16_t count = 0;
        if (!Das::Core::IPC::DeserializeValue(
                std::span<const uint8_t>(data),
                offset,
                count))
        {
            return 0;
        }
        return count;
    }

    TEST_F(PathTraversalTest, EmptyPathListsPluginDir)
    {
        IpcCommandResponse resp{};
        ASSERT_EQ(CallListFile("", resp), DAS_S_OK);

        uint16_t count = ParseListFileEntryCount(resp.response_data);
        EXPECT_EQ(count, uint16_t{1});
    }

    // ============================================================
    // UTF-8 path handling
    // ============================================================

    TEST_F(PathTraversalTest, Utf8PathNotRejected)
    {
        auto utf8_dir =
            tmp_dir_ / "subdir" / u8"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
        std::filesystem::create_directories(utf8_dir);

        IpcCommandResponse resp{};
        std::string relative = "subdir/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
        EXPECT_EQ(CallListFile(relative, resp), DAS_S_OK);
    }

} // anonymous namespace
