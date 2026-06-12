/**
 * @file ReadFileTest.cpp
 * @brief Tests for READ_FILE IPC handler
 *
 * Tests file content reading, missing file error, and size limit enforcement.
 */

#include <cstdint>
#include <cstring>
#include <das/Core/IPC/HandshakeSerialization.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "FakeHostContext.h"

namespace
{
    using Das::Core::IPC::IpcCommandResponse;
    using Das::Core::IPC::IpcCommandType;
    using Das::Core::IPC::ObjectId;
    using Das::Core::IPC::SerializeString;
    using Das::Core::IPC::ValidatedIPCMessageHeader;
    using Das::Core::IPC::Host::FakeHostContext;
    using Das::Core::IPC::Host::HostCommandHandlerOptions;
    using Das::Core::IPC::Host::RegisterHostCommandHandlers;

    struct ReadFileTest : public ::testing::Test
    {
        void SetUp() override
        {
            tmp_dir_ =
                std::filesystem::temp_directory_path() / "das_read_file_test";
            std::filesystem::remove_all(tmp_dir_);
            std::filesystem::create_directories(tmp_dir_);

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
    // Valid file reading
    // ============================================================

    /**
     * @brief Write content to a file and ensure it is flushed to disk
     */
    void WriteFile(
        const std::filesystem::path& path,
        const std::string&           content)
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs << content;
        ofs.flush();
        ofs.close();
    }

    void WriteBinaryFile(
        const std::filesystem::path& path,
        const void*                  data,
        size_t                       size)
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs.write(
            static_cast<const char*>(data),
            static_cast<std::streamsize>(size));
        ofs.flush();
        ofs.close();
    }

    TEST_F(ReadFileTest, ReadsFileContentCorrectly)
    {
        const std::string content = R"({"name": "test_plugin"})";
        WriteFile(tmp_dir_ / "manifest.json", content);

        IpcCommandResponse resp{};
        ASSERT_EQ(CallReadFile("manifest.json", resp), DAS_S_OK);

        ASSERT_GE(resp.response_data.size(), sizeof(uint32_t));

        uint32_t size = 0;
        std::memcpy(&size, resp.response_data.data(), sizeof(uint32_t));
        EXPECT_EQ(size, static_cast<uint32_t>(content.size()));

        std::string actual(
            resp.response_data.begin() + sizeof(uint32_t),
            resp.response_data.end());
        EXPECT_EQ(actual, content);
    }

    TEST_F(ReadFileTest, ReadsBinaryContent)
    {
        std::vector<uint8_t> binary_data(256);
        for (uint16_t i = 0; i < 256; ++i)
        {
            binary_data[i] = static_cast<uint8_t>(i);
        }

        WriteBinaryFile(
            tmp_dir_ / "binary.bin",
            binary_data.data(),
            binary_data.size());

        IpcCommandResponse resp{};
        ASSERT_EQ(CallReadFile("binary.bin", resp), DAS_S_OK);

        ASSERT_GE(
            resp.response_data.size(),
            sizeof(uint32_t) + binary_data.size());

        uint32_t size = 0;
        std::memcpy(&size, resp.response_data.data(), sizeof(uint32_t));
        EXPECT_EQ(size, static_cast<uint32_t>(binary_data.size()));

        std::vector<uint8_t> actual(
            resp.response_data.begin() + sizeof(uint32_t),
            resp.response_data.end());
        EXPECT_EQ(actual, binary_data);
    }

    TEST_F(ReadFileTest, ReadsSubdirectoryFile)
    {
        std::filesystem::create_directories(tmp_dir_ / "PluginA");

        const std::string content = "plugin content";
        WriteFile(tmp_dir_ / "PluginA" / "data.txt", content);

        IpcCommandResponse resp{};
        ASSERT_EQ(CallReadFile("PluginA/data.txt", resp), DAS_S_OK);

        std::string actual(
            resp.response_data.begin() + sizeof(uint32_t),
            resp.response_data.end());
        EXPECT_EQ(actual, content);
    }

    TEST_F(ReadFileTest, ReadsEmptyFile)
    {
        WriteFile(tmp_dir_ / "empty.txt", "");

        IpcCommandResponse resp{};
        ASSERT_EQ(CallReadFile("empty.txt", resp), DAS_S_OK);

        uint32_t size = 0;
        std::memcpy(&size, resp.response_data.data(), sizeof(uint32_t));
        EXPECT_EQ(size, uint32_t{0});
        EXPECT_EQ(resp.response_data.size(), sizeof(uint32_t));
    }

    // ============================================================
    // Missing file error
    // ============================================================

    TEST_F(ReadFileTest, MissingFile_ReturnsFileNotFound)
    {
        IpcCommandResponse resp{};
        EXPECT_EQ(CallReadFile("nonexistent.json", resp), DAS_E_FILE_NOT_FOUND);
    }

    // ============================================================
    // Size limit enforcement
    // ============================================================

    TEST_F(ReadFileTest, OversizedFile_ReturnsInvalidSize)
    {
        auto              big_file = tmp_dir_ / "big.bin";
        constexpr size_t  kSizeLimit = 4 * 1024 * 1024;
        constexpr size_t  kOversize = kSizeLimit + 1024;
        std::vector<char> zeros(65536, 0);
        {
            std::ofstream ofs(big_file, std::ios::binary);
            for (size_t written = 0; written < kOversize;
                 written += zeros.size())
            {
                size_t to_write = std::min(zeros.size(), kOversize - written);
                ofs.write(zeros.data(), static_cast<std::streamsize>(to_write));
            }
            ofs.flush();
            ofs.close();
        }

        ASSERT_TRUE(std::filesystem::exists(big_file));
        ASSERT_GT(std::filesystem::file_size(big_file), kSizeLimit);

        IpcCommandResponse resp{};
        EXPECT_EQ(CallReadFile("big.bin", resp), DAS_E_INVALID_SIZE);
    }

    // ============================================================
    // Invalid payload handling
    // ============================================================

    TEST_F(ReadFileTest, InvalidPayload_ReturnsInvalidMessageBody)
    {
        auto it = ctx_.handlers.find(
            static_cast<uint32_t>(IpcCommandType::READ_FILE));
        ASSERT_NE(it, ctx_.handlers.end());

        std::vector<uint8_t> bad_payload{0xFF, 0xFF};
        IpcCommandResponse   resp{};
        EXPECT_EQ(
            it->second(ValidatedIPCMessageHeader{}, bad_payload, resp),
            DAS_E_IPC_INVALID_MESSAGE_BODY);
    }

} // anonymous namespace
