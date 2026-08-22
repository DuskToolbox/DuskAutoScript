// PluginZipExtractor 行为测试：本地手写 ZIP 夹具（local header + central
// directory + EOCD + 真实 crc32），覆盖插件安装的落盘结构、目录穿越整档
// 拒绝、原子重装与失败保留、解压炸弹 / CRC 完整性 / 加密与未启用压缩
// 方式条目的 fail-closed 拒绝、UTF-8 文件名正确落盘。
#include <das/Core/ForeignInterfaceHost/PluginZipExtractor.h>

#include <das/_autogen/idl/abi/DasResult.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace DAS::Core::ForeignInterfaceHost;

namespace
{
    // manifest 内容：flat zip 的自动前缀依赖 name 字段。
    std::string ManifestJson(const std::string& version)
    {
        return "{\"guid\":\"00000000-0000-0000-0000-000000010501\","
               "\"name\":\"MyPlugin\",\"version\":\""
               + version + "\"}";
    }

    // ---------------------------------------------------------------------
    // 最小 ZIP 构造器（测试用）：stored + deflate 条目，支持伪造声明字段、
    // 穿越条目、加密 / UTF-8 标志与未启用的压缩方式。
    // ---------------------------------------------------------------------

    void AppendU16(std::string& out, std::uint16_t value)
    {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
    }

    void AppendU32(std::string& out, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
        }
    }

    std::string DeflateRaw(const std::string& input)
    {
        z_stream stream{};
        stream.next_in =
            reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
        stream.avail_in = static_cast<uInt>(input.size());
        const int init_result = deflateInit2(
            &stream,
            Z_DEFAULT_COMPRESSION,
            Z_DEFLATED,
            -MAX_WBITS,
            8,
            Z_DEFAULT_STRATEGY);
        if (init_result != Z_OK)
        {
            return {};
        }
        std::string output;
        output.resize(deflateBound(&stream, input.size()));
        stream.next_out = reinterpret_cast<Bytef*>(output.data());
        stream.avail_out = static_cast<uInt>(output.size());
        const int deflate_result = deflate(&stream, Z_FINISH);
        deflateEnd(&stream);
        if (deflate_result != Z_STREAM_END)
        {
            return {};
        }
        output.resize(stream.total_out);
        return output;
    }

    struct TestZipEntry
    {
        std::string   filename;
        std::string   content;
        bool          deflate = false;
        std::uint32_t declared_uncompressed_size = 0;
        std::uint32_t declared_crc = 0;
        // bit 11（0x0800）= UTF-8 文件名；bit 0（0x0001）= 加密条目。
        std::uint16_t general_flags = 0;
        // 0 stored、8 deflate、12 bzip2（未启用的压缩方式必须被拒绝）。
        std::uint16_t method = 0;
    };

    // 构造一个 ZIP（stored / deflate）。declared_uncompressed_size 非 0 时，
    // 头部的 uncompressed size 字段用该声明值（伪造与真实内容不符的声明，
    // 用于解压炸弹 / 大小造假测试）；declared_crc 非 0 时，local header 与
    // central directory 的 CRC 字段都写该声明值（用于损坏包拒绝测试）；
    // general_flags 写入两处头部标志字段；method 非 0 时覆盖两处头部压缩
    // 方式字段。
    std::string BuildTestZip(const std::vector<TestZipEntry>& entries)
    {
        std::string local_section;
        std::string central_section;
        for (const auto& entry : entries)
        {
            const auto data =
                entry.deflate ? DeflateRaw(entry.content) : entry.content;
            const auto declared_size =
                entry.declared_uncompressed_size != 0
                    ? entry.declared_uncompressed_size
                    : static_cast<std::uint32_t>(entry.content.size());
            const std::uint32_t actual_crc = crc32(
                0,
                reinterpret_cast<const Bytef*>(entry.content.data()),
                static_cast<uInt>(entry.content.size()));
            const auto crc =
                entry.declared_crc != 0 ? entry.declared_crc : actual_crc;
            const auto method =
                entry.method != 0
                    ? entry.method
                    : static_cast<std::uint16_t>(entry.deflate ? 8 : 0);

            const auto local_header_offset =
                static_cast<std::uint32_t>(local_section.size());

            // Local file header
            AppendU32(local_section, 0x04034B50);
            AppendU16(local_section, 20); // version needed
            AppendU16(local_section, entry.general_flags);
            AppendU16(local_section, method);
            AppendU16(local_section, 0); // mod time
            AppendU16(local_section, 0); // mod date
            AppendU32(local_section, crc);
            AppendU32(local_section, static_cast<std::uint32_t>(data.size()));
            AppendU32(local_section, declared_size);
            AppendU16(
                local_section,
                static_cast<std::uint16_t>(entry.filename.size()));
            AppendU16(local_section, 0); // extra length
            local_section += entry.filename;
            local_section += data;

            // Central directory entry
            AppendU32(central_section, 0x02014B50);
            AppendU16(central_section, 20); // version made by
            AppendU16(central_section, 20); // version needed
            AppendU16(central_section, entry.general_flags);
            AppendU16(central_section, method);
            AppendU16(central_section, 0); // mod time
            AppendU16(central_section, 0); // mod date
            AppendU32(central_section, crc);
            AppendU32(central_section, static_cast<std::uint32_t>(data.size()));
            AppendU32(central_section, declared_size);
            AppendU16(
                central_section,
                static_cast<std::uint16_t>(entry.filename.size()));
            AppendU16(central_section, 0); // extra length
            AppendU16(central_section, 0); // comment length
            AppendU16(central_section, 0); // disk number start
            AppendU16(central_section, 0); // internal attrs
            AppendU32(central_section, 0); // external attrs
            AppendU32(central_section, local_header_offset);
            central_section += entry.filename;
        }

        // End of central directory
        const auto central_size =
            static_cast<std::uint32_t>(central_section.size());
        std::string eocd;
        AppendU32(eocd, 0x06054B50);
        AppendU16(eocd, 0); // disk number
        AppendU16(eocd, 0); // disk with central dir
        AppendU16(eocd, static_cast<std::uint16_t>(entries.size()));
        AppendU16(eocd, static_cast<std::uint16_t>(entries.size()));
        AppendU32(eocd, central_size);
        AppendU32(eocd, static_cast<std::uint32_t>(local_section.size()));
        AppendU16(eocd, 0); // comment length

        return local_section + central_section + eocd;
    }

    // ---------------------------------------------------------------------
    // 目录夹具与断言辅助
    // ---------------------------------------------------------------------

    class TempPluginDir
    {
    public:
        TempPluginDir()
        {
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = std::filesystem::temp_directory_path()
                    / ("das-plugin-zip-" + std::to_string(stamp));
            std::filesystem::create_directories(root_);
        }

        TempPluginDir(const TempPluginDir&) = delete;
        TempPluginDir& operator=(const TempPluginDir&) = delete;

        ~TempPluginDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
        }

        [[nodiscard]]
        const std::filesystem::path& Dir() const
        {
            return root_;
        }

    private:
        std::filesystem::path root_;
    };

    // 安装成功与失败路径都不得在 plugin_dir 下遗留 .tmp_install_ 临时目录。
    bool HasTmpInstallResidue(const std::filesystem::path& dir)
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            const auto name = entry.path().filename().string();
            if (name.rfind(".tmp_install_", 0) == 0)
            {
                return true;
            }
        }
        return false;
    }

    std::string ReadFileContent(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
    }

    // ---------------------------------------------------------------------
    // 既有行为回归锁（迁移前即应通过）
    // ---------------------------------------------------------------------

    TEST(PluginZip, InstallPluginFlatZipAutoPrefixesWithManifestName)
    {
        // flat zip（根级 manifest + 文件）自动按 manifest 的 name 前缀包装，
        // 安装到 plugin_dir/<name>/ 下。
        TempPluginDir plugin_dir;
        const auto    zip = BuildTestZip({
            TestZipEntry{
                .filename = "manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{.filename = "script.txt", .content = "print('hello')"},
        });

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_S_OK);
        const auto installed = plugin_dir.Dir() / "MyPlugin";
        EXPECT_TRUE(std::filesystem::exists(installed / "manifest.json"));
        EXPECT_EQ(
            ReadFileContent(installed / "manifest.json"),
            ManifestJson("1"));
        EXPECT_EQ(ReadFileContent(installed / "script.txt"), "print('hello')");
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginPackagedZipPreservesStructure)
    {
        // packaged zip（*/manifest.json）保持内部结构安装到
        // plugin_dir/<顶层目录名>/ 下。
        TempPluginDir plugin_dir;
        const auto    zip = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{
                .filename = "MyPlugin/data/payload.bin",
                .content = "nested-data"},
        });

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_S_OK);
        const auto installed = plugin_dir.Dir() / "MyPlugin";
        EXPECT_TRUE(std::filesystem::exists(installed / "manifest.json"));
        EXPECT_EQ(
            ReadFileContent(installed / "data" / "payload.bin"),
            "nested-data");
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginRejectsTraversalEntry)
    {
        // 含 "../" 穿越条目 —— 整档拒绝，任何文件都不得落盘。
        TempPluginDir plugin_dir;
        const auto    zip = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{.filename = "../evil.txt", .content = "escape"},
        });

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_FALSE(std::filesystem::exists(plugin_dir.Dir() / "MyPlugin"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
        EXPECT_FALSE(
            std::filesystem::exists(
                std::filesystem::temp_directory_path() / "evil.txt"));
    }

    TEST(PluginZip, InstallPluginRejectsAbsoluteAndBackslashTraversal)
    {
        // 前导 '/' 的绝对路径条目与反斜杠 ".." 分量条目同样整档拒绝。
        TempPluginDir plugin_dir;
        const auto    zip = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{.filename = "/abs.txt", .content = "abs"},
            TestZipEntry{
                .filename = "MyPlugin\\..\\evil.txt",
                .content = "evil"},
        });

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_FALSE(std::filesystem::exists(plugin_dir.Dir() / "MyPlugin"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginRejectsEmptyZip)
    {
        // 空 zip（0 条目）拒绝，不创建任何安装目录。
        TempPluginDir plugin_dir;
        const auto    zip = BuildTestZip({});

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_FALSE(std::filesystem::exists(plugin_dir.Dir() / "MyPlugin"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginRejectsGarbageData)
    {
        // 非 zip 字节直接拒绝（垃圾长度不小于 EOCD 记录长度，锁定
        // 「按返回码拒绝」的行为本身）。
        TempPluginDir plugin_dir;

        const auto result = InstallPlugin(
            plugin_dir.Dir(),
            "this-is-definitely-not-a-valid-zip-archive-payload");

        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_FALSE(std::filesystem::exists(plugin_dir.Dir() / "MyPlugin"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginRejectsUnsupportedMethod)
    {
        // 声明 bzip2 压缩方式的条目（构建未启用 bzip2）必须 fail-closed 拒绝。
        TempPluginDir plugin_dir;
        const auto    zip = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{
                .filename = "MyPlugin/bad.bin",
                .content = "bzip2-payload",
                .method = 12},
        });

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_FALSE(std::filesystem::exists(plugin_dir.Dir() / "MyPlugin"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginAtomicReinstallReplacesExisting)
    {
        // 重装同名插件：旧版本移走、新版本就位，旧目录与临时目录全清理。
        TempPluginDir plugin_dir;
        const auto    zip_v1 = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{.filename = "MyPlugin/file.txt", .content = "v1"},
        });
        const auto    zip_v2 = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("2")},
            TestZipEntry{.filename = "MyPlugin/file.txt", .content = "v2"},
        });

        ASSERT_EQ(InstallPlugin(plugin_dir.Dir(), zip_v1), DAS_S_OK);
        const auto result = InstallPlugin(plugin_dir.Dir(), zip_v2);

        EXPECT_EQ(result, DAS_S_OK);
        const auto installed = plugin_dir.Dir() / "MyPlugin";
        EXPECT_EQ(ReadFileContent(installed / "file.txt"), "v2");
        EXPECT_EQ(
            ReadFileContent(installed / "manifest.json"),
            ManifestJson("2"));
        EXPECT_FALSE(
            std::filesystem::exists(plugin_dir.Dir() / "MyPlugin.old"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginFailedInstallLeavesExistingIntact)
    {
        // 已安装插件上投递损坏 zip：安装失败，既有内容原样保留。
        TempPluginDir plugin_dir;
        const auto    zip_v1 = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{.filename = "MyPlugin/file.txt", .content = "v1"},
        });

        ASSERT_EQ(InstallPlugin(plugin_dir.Dir(), zip_v1), DAS_S_OK);
        const auto result = InstallPlugin(
            plugin_dir.Dir(),
            "this-is-definitely-not-a-valid-zip-archive-payload");

        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        const auto installed = plugin_dir.Dir() / "MyPlugin";
        EXPECT_EQ(ReadFileContent(installed / "file.txt"), "v1");
        EXPECT_EQ(
            ReadFileContent(installed / "manifest.json"),
            ManifestJson("1"));
        EXPECT_FALSE(
            std::filesystem::exists(plugin_dir.Dir() / "MyPlugin.old"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    // ---------------------------------------------------------------------
    // 新能力用例（迁移前缺失的防护，RED 基线）
    // ---------------------------------------------------------------------

    TEST(PluginZip, InstallPluginRejectsStoredEntryWithCorruptCrc)
    {
        // stored 条目伪造声明 CRC —— 完整性校验必须击穿并整档拒绝。
        TempPluginDir plugin_dir;
        const auto    zip = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{
                .filename = "MyPlugin/bad.bin",
                .content = "stored-payload",
                .declared_crc = 0xDEADBEEF},
        });

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_FALSE(std::filesystem::exists(plugin_dir.Dir() / "MyPlugin"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginRejectsDeflateEntryWithCorruptCrc)
    {
        // deflate 条目伪造声明 CRC —— 同样必须击穿并整档拒绝。
        TempPluginDir plugin_dir;
        const auto    zip = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{
                .filename = "MyPlugin/bad.bin",
                .content = "deflate-payload",
                .deflate = true,
                .declared_crc = 0xDEADBEEF},
        });

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_FALSE(std::filesystem::exists(plugin_dir.Dir() / "MyPlugin"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginRejectsDeclaredSizeOverLimit)
    {
        // 声明解压大小超上限（stored 条目，声明远超真实内容）—— 开读前
        // 拒绝，不触发大分配。
        TempPluginDir plugin_dir;
        const auto    zip = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{
                .filename = "MyPlugin/huge.bin",
                .content = "tiny",
                .declared_uncompressed_size = 1024u * 1024u * 1024u},
        });

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_FALSE(std::filesystem::exists(plugin_dir.Dir() / "MyPlugin"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginRejectsActualSizeExceedsDeclared)
    {
        // 实际内容大于声明大小（stored 条目，声明偏小）—— 读取循环内
        // 累计超声明值即时拒绝。
        TempPluginDir plugin_dir;
        const auto    zip = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{
                .filename = "MyPlugin/liar.bin",
                .content = "this-content-is-much-longer-than-the-declared-size",
                .declared_uncompressed_size = 4},
        });

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_FALSE(std::filesystem::exists(plugin_dir.Dir() / "MyPlugin"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginRejectsEncryptedEntry)
    {
        // 加密标志条目（bit 0，无密码）—— 必须被拒绝，不尝试解密。
        TempPluginDir plugin_dir;
        const auto    zip = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{
                .filename = "MyPlugin/secret.bin",
                .content = "secret",
                .general_flags = 0x0001},
        });

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
        EXPECT_FALSE(std::filesystem::exists(plugin_dir.Dir() / "MyPlugin"));
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

    TEST(PluginZip, InstallPluginExtractsUtf8FilenameCorrectly)
    {
        // bit 11 UTF-8 文件名条目：非 ASCII 名必须按 UTF-8 字节原样落盘
        // （断言经 u8string 逐字节比对，内容完整）。
        TempPluginDir     plugin_dir;
        const std::string utf8_entry_name =
            "MyPlugin/\xE6\x8F\x92\xE4\xBB\xB6.txt"; // 插件.txt（UTF-8 字节）
        const auto zip = BuildTestZip({
            TestZipEntry{
                .filename = "MyPlugin/manifest.json",
                .content = ManifestJson("1")},
            TestZipEntry{
                .filename = utf8_entry_name,
                .content = "utf8-content",
                .general_flags = 0x0800},
        });

        const auto result = InstallPlugin(plugin_dir.Dir(), zip);

        EXPECT_EQ(result, DAS_S_OK);
        const auto installed = plugin_dir.Dir() / "MyPlugin";
        EXPECT_TRUE(std::filesystem::exists(installed / "manifest.json"));

        const std::string   expected_bytes = "\xE6\x8F\x92\xE4\xBB\xB6.txt";
        const std::u8string expected_name(
            expected_bytes.begin(),
            expected_bytes.end());
        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(installed))
        {
            if (entry.path().filename().u8string() == expected_name)
            {
                found = true;
                EXPECT_EQ(ReadFileContent(entry.path()), "utf8-content");
            }
        }
        EXPECT_TRUE(found);
        EXPECT_FALSE(HasTmpInstallResidue(plugin_dir.Dir()));
    }

} // namespace
