/**
 * @file ListFileTest.cpp
 * @brief Tests for LIST_FILE IPC handler
 *
 * Tests both recursive and non-recursive directory listing,
 * response format correctness, and error handling.
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

    /**
     * @brief Helper to deserialize a LIST_FILE response
     *
     * Response format:
     *   SerializeString(working_path)
     *   uint16_t entry_count
     *   [uint8_t type + SerializeString(name) + SerializeString(abs_path)][]
     */
    struct ListFileResponse
    {
        std::string working_path;
        struct Entry
        {
            uint8_t     type;
            std::string name;
            std::string abs_path;
        };
        std::vector<Entry> entries;
    };

    bool ParseListFileResponse(
        const std::vector<uint8_t>& data,
        ListFileResponse&           out)
    {
        size_t offset = 0;
        auto   span = std::span<const uint8_t>(data);

        if (!Das::Core::IPC::DeserializeString(span, offset, out.working_path))
        {
            return false;
        }

        uint16_t count = 0;
        if (!Das::Core::IPC::DeserializeValue(span, offset, count))
        {
            return false;
        }

        out.entries.resize(count);
        for (uint16_t i = 0; i < count; ++i)
        {
            if (!Das::Core::IPC::DeserializeValue(
                    span,
                    offset,
                    out.entries[i].type))
            {
                return false;
            }
            if (!Das::Core::IPC::DeserializeString(
                    span,
                    offset,
                    out.entries[i].name))
            {
                return false;
            }
            if (!Das::Core::IPC::DeserializeString(
                    span,
                    offset,
                    out.entries[i].abs_path))
            {
                return false;
            }
        }
        return true;
    }

    struct ListFileTest : public ::testing::Test
    {
        void SetUp() override
        {
            tmp_dir_ =
                std::filesystem::temp_directory_path() / "das_list_file_test";
            std::filesystem::remove_all(tmp_dir_);
            std::filesystem::create_directories(tmp_dir_);
            std::filesystem::create_directories(tmp_dir_ / "PluginA");
            std::filesystem::create_directories(tmp_dir_ / "PluginB");

            {
                std::ofstream(tmp_dir_ / "PluginA" / "manifest.json") << "{}";
                std::ofstream(tmp_dir_ / "PluginA" / "readme.txt") << "hello";
                std::ofstream(tmp_dir_ / "PluginB" / "manifest.json") << "{}";
                std::ofstream(tmp_dir_ / "root.txt") << "root file";
            }

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
            bool                recursive,
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
            SerializeValue(payload, static_cast<uint8_t>(recursive ? 1 : 0));

            return it->second(ValidatedIPCMessageHeader{}, payload, response);
        }

        FakeHostContext       ctx_;
        std::filesystem::path tmp_dir_;
    };

    // ============================================================
    // Non-recursive listing
    // ============================================================

    TEST_F(ListFileTest, NonRecursive_RootDir_ListsEntries)
    {
        IpcCommandResponse resp{};
        ASSERT_EQ(CallListFile("", false, resp), DAS_S_OK);

        ListFileResponse parsed;
        ASSERT_TRUE(ParseListFileResponse(resp.response_data, parsed));

        EXPECT_EQ(parsed.entries.size(), size_t{3});

        int dir_count = 0;
        int file_count = 0;
        for (const auto& e : parsed.entries)
        {
            if (e.type == 1)
            {
                ++dir_count;
            }
            else
            {
                ++file_count;
            }
        }
        EXPECT_EQ(dir_count, 2);
        EXPECT_EQ(file_count, 1);
    }

    TEST_F(ListFileTest, NonRecursive_SubDir_ListsOnlySubEntries)
    {
        IpcCommandResponse resp{};
        ASSERT_EQ(CallListFile("PluginA", false, resp), DAS_S_OK);

        ListFileResponse parsed;
        ASSERT_TRUE(ParseListFileResponse(resp.response_data, parsed));

        EXPECT_EQ(parsed.entries.size(), size_t{2});
        for (const auto& e : parsed.entries)
        {
            EXPECT_EQ(e.type, uint8_t{0});
        }
    }

    // ============================================================
    // Recursive listing
    // ============================================================

    TEST_F(ListFileTest, Recursive_RootDir_ListsAllDescendants)
    {
        IpcCommandResponse resp{};
        ASSERT_EQ(CallListFile("", true, resp), DAS_S_OK);

        ListFileResponse parsed;
        ASSERT_TRUE(ParseListFileResponse(resp.response_data, parsed));

        EXPECT_GE(parsed.entries.size(), size_t{5});

        bool has_plugin_a_manifest = false;
        for (const auto& e : parsed.entries)
        {
            if (e.name == "manifest.json"
                && e.abs_path.find("PluginA") != std::string::npos)
            {
                has_plugin_a_manifest = true;
            }
        }
        EXPECT_TRUE(has_plugin_a_manifest);
    }

    // ============================================================
    // Response format correctness
    // ============================================================

    TEST_F(ListFileTest, ResponseContainsWorkingPath)
    {
        IpcCommandResponse resp{};
        ASSERT_EQ(CallListFile("", false, resp), DAS_S_OK);

        ListFileResponse parsed;
        ASSERT_TRUE(ParseListFileResponse(resp.response_data, parsed));

        auto        expected_u8 = tmp_dir_.u8string();
        std::string expected(
            reinterpret_cast<const char*>(expected_u8.data()),
            expected_u8.size());
        EXPECT_EQ(parsed.working_path, expected);
    }

    TEST_F(ListFileTest, ResponseEntryHasTypeFileNameAndAbsPath)
    {
        IpcCommandResponse resp{};
        ASSERT_EQ(CallListFile("PluginA", false, resp), DAS_S_OK);

        ListFileResponse parsed;
        ASSERT_TRUE(ParseListFileResponse(resp.response_data, parsed));
        ASSERT_FALSE(parsed.entries.empty());

        bool found_manifest = false;
        for (const auto& e : parsed.entries)
        {
            if (e.name == "manifest.json")
            {
                found_manifest = true;
                EXPECT_EQ(e.type, uint8_t{0});

                auto expected_abs =
                    (tmp_dir_ / "PluginA" / "manifest.json").u8string();
                std::string expected(
                    reinterpret_cast<const char*>(expected_abs.data()),
                    expected_abs.size());
                EXPECT_EQ(e.abs_path, expected);
            }
        }
        EXPECT_TRUE(found_manifest);
    }

    // ============================================================
    // Error handling
    // ============================================================

    TEST_F(ListFileTest, EmptyPluginDir_NotRegisteredWhenNoDir)
    {
        FakeHostContext           no_dir_ctx;
        HostCommandHandlerOptions opts{};
        opts.load_plugin = [](const std::filesystem::path&)
            -> Das::Utils::Expected<DAS::DasPtr<IDasBase>>
        { return DAS::DasPtr<IDasBase>(static_cast<IDasBase*>(nullptr)); };
        opts.plugin_dir = "";

        ASSERT_EQ(
            RegisterHostCommandHandlers(&no_dir_ctx, std::move(opts)),
            DAS_S_OK);

        EXPECT_EQ(
            no_dir_ctx.handlers.find(
                static_cast<uint32_t>(IpcCommandType::LIST_FILE)),
            no_dir_ctx.handlers.end());
    }

    TEST_F(ListFileTest, InvalidPayload_ReturnsInvalidMessageBody)
    {
        auto it = ctx_.handlers.find(
            static_cast<uint32_t>(IpcCommandType::LIST_FILE));
        ASSERT_NE(it, ctx_.handlers.end());

        std::vector<uint8_t> bad_payload{0xFF, 0xFF};
        IpcCommandResponse   resp{};
        EXPECT_EQ(
            it->second(ValidatedIPCMessageHeader{}, bad_payload, resp),
            DAS_E_IPC_INVALID_MESSAGE_BODY);
    }

} // anonymous namespace
