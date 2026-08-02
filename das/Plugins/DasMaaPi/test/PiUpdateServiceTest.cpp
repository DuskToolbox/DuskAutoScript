#include "../src/PiUpdateService.h"
#include "../src/PiHttpClient.h"

#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/_autogen/idl/abi/DasResult.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;

    // Fake PiHttpClient implementing the network seam (RESEARCH Pitfall 2).
    // Production passes BeastHttpClient; tests inject this to serve canned
    // responses per URL substring with zero external network. Records every
    // call (url, status, body, proxy_url) so tests can assert routing and
    // proxy forwarding.
    class FakePiHttpClient final : public PiHttpClient
    {
    public:
        struct Route
        {
            std::string path_fragment;
            std::string body;
            int         status = 200;
        };

        struct Call
        {
            std::string url;
            int         status = 0;
            std::string body;
            std::string proxy_url;
        };

        void AddRoute(
            std::string path_fragment,
            std::string body,
            int         status = 200)
        {
            routes_.push_back(
                Route{std::move(path_fragment), std::move(body), status});
        }

        // When true, every Get/Head fails (simulated network failure).
        void SetNetworkFailure(bool fail) { fail_all_ = fail; }

        const std::vector<Call>& Calls() const { return calls_; }

        std::vector<std::string> Urls() const
        {
            std::vector<std::string> urls;
            for (const auto& call : calls_)
            {
                urls.push_back(call.url);
            }
            return urls;
        }

        int CountRequestsWith(std::string_view fragment) const
        {
            int count = 0;
            for (const auto& call : calls_)
            {
                if (call.url.find(fragment) != std::string::npos)
                {
                    ++count;
                }
            }
            return count;
        }

        DasResult Get(
            const std::string& url,
            std::string&       out_body,
            int&               out_status,
            const std::string& proxy_url) override
        {
            return Dispatch(url, out_body, out_status, proxy_url);
        }

        DasResult Head(
            const std::string& url,
            int&               out_status,
            const std::string& proxy_url) override
        {
            std::string body;
            return Dispatch(url, body, out_status, proxy_url);
        }

    private:
        DasResult Dispatch(
            const std::string& url,
            std::string&       out_body,
            int&               out_status,
            const std::string& proxy_url)
        {
            calls_.push_back(Call{url, 0, out_body, proxy_url});
            if (fail_all_)
            {
                return DAS_E_FAIL;
            }
            for (const auto& route : routes_)
            {
                if (url.find(route.path_fragment) != std::string::npos)
                {
                    out_body = route.body;
                    out_status = route.status;
                    calls_.back().status = route.status;
                    calls_.back().body = route.body;
                    return DAS_S_OK;
                }
            }
            out_status = 404;
            calls_.back().status = 404;
            return DAS_S_OK;
        }

        std::vector<Route> routes_;
        std::vector<Call>  calls_;
        bool               fail_all_ = false;
    };

    // Temp project dir with interface.json. Unique name per instance so
    // parallel test instances (and their _backup_ / .update siblings) never
    // collide.
    class TempPiProject
    {
    public:
        explicit TempPiProject(std::string name = {})
        {
            static std::atomic<int> counter{0};
            if (name.empty())
            {
                name = "PiUpdate_" + std::to_string(counter.fetch_add(1));
            }
            name_ = std::move(name);
            dir_ = std::filesystem::temp_directory_path() / name_;
            Cleanup();
            std::error_code ec;
            std::filesystem::create_directories(dir_, ec);
        }

        ~TempPiProject() { Cleanup(); }

        const std::filesystem::path& Dir() const { return dir_; }

        void WriteInterface(const std::string& json)
        {
            std::ofstream file(dir_ / "interface.json", std::ios::binary);
            file << json;
        }

    private:
        void Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir_, ec);
            // 清理兄弟目录：{name}_backup_{version} + {name}.update。
            // 各 ctest 用例独立进程运行，static counter 每次从 0 起，需按
            // 前缀清掉上次用例残留的 backup。
            const auto parent = dir_.parent_path();
            const auto backup_prefix = name_ + "_backup_";
            const auto update_name = name_ + ".update";
            for (const auto& entry : std::filesystem::directory_iterator(
                     parent,
                     std::filesystem::directory_options::skip_permission_denied,
                     ec))
            {
                const auto leaf = entry.path().filename().string();
                if (leaf.rfind(backup_prefix, 0) == 0 || leaf == update_name)
                {
                    std::filesystem::remove_all(entry.path(), ec);
                }
            }
        }

        std::string           name_;
        std::filesystem::path dir_;
    };

    std::string MakeInterface(
        const std::string& version,
        const std::string& github,
        const std::string& rid,
        const std::string& name = "test-project")
    {
        return R"({
            "interface_version": 2,
            "name": ")"
               + name + R"(",
            "version": ")"
               + version + R"(",
            "github": ")"
               + github + R"(",
            "mirrorchyan_rid": ")"
               + rid + R"("
        })";
    }

    std::string MirrorResponse(
        const std::string& version_name,
        int                code = 0,
        const std::string& url = "https://mirror.example/package.zip",
        const std::string& sha256 = "sha256-abc",
        const std::string& update_type = "full")
    {
        return R"({"code":)" + std::to_string(code)
               + R"(,"msg":"ok","data":{"version_name":")" + version_name
               + R"(","url":")" + url + R"(","sha256":")" + sha256
               + R"(","release_note":"release notes","update_type":")"
               + update_type + R"("}})";
    }

    std::string GithubResponse(
        const std::string& tag,
        const std::string& asset_name = "test-project-win-x86_64-v1.2.0.zip")
    {
        return R"({"tag_name":")" + tag
               + R"(","body":"github notes","assets":[{"name":")" + asset_name
               + R"(","size":123,"browser_download_url":"https://github.com/owner/repo/releases/download/)"
               + tag + R"(/)" + asset_name + R"("}]})";
    }

    // ---------------------------------------------------------------------
    // 最小 ZIP 构造器（测试用）：stored + deflate 条目 + 目录穿越条目。
    // 参考 PluginZipExtractor.cpp 的 ZIP 二进制格式。
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
        std::string filename;
        std::string content;
        bool        deflate = false;
    };

    // 构造一个合法的 ZIP（stored / deflate），支持故意含 ../ 的穿越条目。
    std::string BuildTestZip(const std::vector<TestZipEntry>& entries)
    {
        std::string local_section;
        std::string central_section;
        for (const auto& entry : entries)
        {
            const auto data =
                entry.deflate ? DeflateRaw(entry.content) : entry.content;
            const std::uint32_t crc = crc32(
                0,
                reinterpret_cast<const Bytef*>(entry.content.data()),
                static_cast<uInt>(entry.content.size()));

            const auto local_header_offset =
                static_cast<std::uint32_t>(local_section.size());

            // Local file header
            AppendU32(local_section, 0x04034B50);
            AppendU16(local_section, 20); // version needed
            AppendU16(local_section, 0);  // flags
            AppendU16(local_section, entry.deflate ? 8 : 0);
            AppendU16(local_section, 0); // mod time
            AppendU16(local_section, 0); // mod date
            AppendU32(local_section, crc);
            AppendU32(local_section, static_cast<std::uint32_t>(data.size()));
            AppendU32(
                local_section,
                static_cast<std::uint32_t>(entry.content.size()));
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
            AppendU16(central_section, 0);  // flags
            AppendU16(central_section, entry.deflate ? 8 : 0);
            AppendU16(central_section, 0); // mod time
            AppendU16(central_section, 0); // mod date
            AppendU32(central_section, crc);
            AppendU32(central_section, static_cast<std::uint32_t>(data.size()));
            AppendU32(
                central_section,
                static_cast<std::uint32_t>(entry.content.size()));
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
} // namespace

TEST(PiUpdateService, CheckUpdateMirrorChyanWins)
{
    FakePiHttpClient http;
    http.AddRoute("/api/resources/rid-1/latest", MirrorResponse("1.2.0"));

    TempPiProject project;
    project.WriteInterface(
        MakeInterface("1.0.0", "https://github.com/owner/repo", "rid-1"));

    PiUpdateInfo info;
    const auto result = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(info.has_update);
    EXPECT_EQ(info.version_name, "1.2.0");
    EXPECT_EQ(info.download_url, "https://mirror.example/package.zip");
    EXPECT_TRUE(info.sha256.has_value());
    EXPECT_FALSE(info.error_code.has_value());
    // MirrorChyan won — GitHub was never contacted.
    EXPECT_EQ(http.CountRequestsWith("/repos/owner/repo"), 0);
    EXPECT_EQ(http.CountRequestsWith("/api/resources/rid-1/latest"), 1);
}

TEST(PiUpdateService, CheckUpdateNoRidFallsBackToGithub)
{
    FakePiHttpClient http;
    http.AddRoute(
        "/repos/owner/repo/releases/latest",
        GithubResponse("v1.2.0"));

    TempPiProject project;
    project.WriteInterface(
        MakeInterface("1.0.0", "https://github.com/owner/repo", ""));

    PiUpdateInfo info;
    const auto result = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(info.has_update);
    EXPECT_EQ(info.version_name, "v1.2.0");
    EXPECT_EQ(
        info.download_url,
        "https://github.com/owner/repo/releases/download/v1.2.0/"
        "test-project-win-x86_64-v1.2.0.zip");
}

TEST(PiUpdateService, CheckUpdateMirrorCodeNonzeroReturnsPartialInfo)
{
    FakePiHttpClient http;
    // code != 0 but data.version_name exists → partial info + error_code.
    http.AddRoute(
        "/api/resources/rid-2/latest",
        MirrorResponse("1.2.0", /*code=*/7002, /*url=*/""));

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", "rid-2"));

    PiUpdateInfo info;
    const auto result = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(info.has_update);
    EXPECT_TRUE(info.error_code.has_value());
    EXPECT_EQ(*info.error_code, 7002);
    EXPECT_TRUE(info.download_url.empty());
}

TEST(PiUpdateService, CheckUpdateNetworkFailureReturnsCheckFailed)
{
    FakePiHttpClient http;
    http.SetNetworkFailure(true);

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", "rid-3"));

    PiUpdateInfo info;
    const auto result = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_CHECK_FAILED);
}

TEST(PiUpdateService, CheckUpdateNetworkFailureFallsBackToGithub)
{
    FakePiHttpClient http;
    // MirrorChyan network-fails (no route → 404), GitHub provides the update.
    http.AddRoute(
        "/repos/owner/repo/releases/latest",
        GithubResponse("v1.2.0"));

    TempPiProject project;
    project.WriteInterface(
        MakeInterface("1.0.0", "https://github.com/owner/repo", "rid-4"));

    PiUpdateInfo info;
    const auto result = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(info.has_update);
    EXPECT_NE(info.download_url.find("github.com/"), std::string::npos);
}

TEST(PiUpdateService, CheckUpdateForwardsProxyUrlToSeam)
{
    FakePiHttpClient http;
    http.AddRoute("/api/resources/rid-proxy/latest", MirrorResponse("1.2.0"));

    TempPiProject project;
    project.WriteInterface(
        MakeInterface("1.0.0", "https://github.com/owner/repo", "rid-proxy"));

    constexpr std::string_view kProxy = "http://127.0.0.1:8899";
    PiUpdateInfo               info;
    const auto                 result =
        PiUpdateService::CheckUpdate(project.Dir(), http, info, kProxy);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(info.has_update);
    // Every request through the seam carried the proxy URL.
    ASSERT_FALSE(http.Calls().empty());
    for (const auto& call : http.Calls())
    {
        EXPECT_EQ(call.proxy_url, kProxy);
    }
}

TEST(PiUpdateService, CheckUpdateMissingVersionFails)
{
    TempPiProject project;
    project.WriteInterface(R"({
        "interface_version": 2,
        "name": "test-project"
    })");

    FakePiHttpClient http;
    PiUpdateInfo     info;
    const auto result = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_CHECK_FAILED);
}

TEST(PiUpdateService, CheckUpdateIncrementalDegradesToGithubFull)
{
    FakePiHttpClient http;
    // MirrorChyan returns an incremental package — D-89-03 says only consume
    // full updates, so the mirror download link is NOT adopted.
    http.AddRoute(
        "/api/resources/rid-inc/latest",
        MirrorResponse(
            "1.2.0",
            /*code=*/0,
            "https://mirror.example/incremental.zip",
            "sha256-inc",
            "incremental"));
    http.AddRoute(
        "/repos/owner/repo/releases/latest",
        GithubResponse("v1.2.0"));

    TempPiProject project;
    project.WriteInterface(
        MakeInterface("1.0.0", "https://github.com/owner/repo", "rid-inc"));

    PiUpdateInfo info;
    const auto result = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(info.has_update);
    // GitHub full-package URL adopted, not the incremental mirror link.
    EXPECT_EQ(
        info.download_url,
        "https://github.com/owner/repo/releases/download/v1.2.0/"
        "test-project-win-x86_64-v1.2.0.zip");
}

// ---------------------------------------------------------------------
// ApplyUpdate（D-89-03 下载 → 安全解压 → 备份 → 原子替换 → 回滚）
// ---------------------------------------------------------------------

TEST(PiUpdateService, ApplyUpdateDownloadsExtractsBacksUpAndReplaces)
{
    FakePiHttpClient http;
    http.AddRoute(
        "/releases/download/v1.2.0/package.zip",
        BuildTestZip({
            TestZipEntry{"interface.json", "{\"version\":\"1.2.0\"}"},
            TestZipEntry{"config.json", "{\"speed\":10}"},
            TestZipEntry{
                "assets/image.png",
                "fake-image-bytes",
                /*deflate=*/true},
        }));

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url =
        "https://github.com/owner/repo/releases/download/v1.2.0/package.zip";

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);

    EXPECT_EQ(result, DAS_S_OK);

    // 替换后 pi_dir 内容 = 新 zip 内容。
    std::ifstream new_interface(project.Dir() / "interface.json");
    std::string   content(
        (std::istreambuf_iterator<char>(new_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "{\"version\":\"1.2.0\"}");
    EXPECT_TRUE(std::filesystem::exists(project.Dir() / "config.json"));
    EXPECT_TRUE(std::filesystem::exists(project.Dir() / "assets/image.png"));

    // 备份目录存在（{pi_dir}_backup_{oldVersion}）。
    const auto backup_dir =
        std::filesystem::path(project.Dir().string() + "_backup_1.0.0");
    EXPECT_TRUE(std::filesystem::is_directory(backup_dir));
    EXPECT_TRUE(std::filesystem::exists(backup_dir / "interface.json"));

    // .update 临时目录已清理。
    const auto update_root = project.Dir().parent_path()
                             / (project.Dir().filename().string() + ".update");
    EXPECT_FALSE(std::filesystem::exists(update_root));
}

TEST(PiUpdateService, ApplyUpdateRejectsTraversalEntry)
{
    // 含一个故意 ../evil.txt 条目 —— 目录穿越防护必须拒绝（T-89-02-01）。
    FakePiHttpClient http;
    http.AddRoute(
        "/download/evil.zip",
        BuildTestZip({
            TestZipEntry{"interface.json", "{\"version\":\"1.2.0\"}"},
            TestZipEntry{"../evil.txt", "escape"},
        }));

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/evil.zip";

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_APPLY_FAILED);

    // 旧内容未被替换（未生成 backup / 未动 pi_dir 之外）。
    EXPECT_TRUE(std::filesystem::exists(project.Dir() / "interface.json"));
    std::ifstream old_interface(project.Dir() / "interface.json");
    std::string   content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
    EXPECT_FALSE(
        std::filesystem::exists(
            std::filesystem::temp_directory_path() / "evil.txt"));
    // .update 临时目录已清理。
    const auto update_root = project.Dir().parent_path()
                             / (project.Dir().filename().string() + ".update");
    EXPECT_FALSE(std::filesystem::exists(update_root));
}

TEST(PiUpdateService, ApplyUpdateInvalidZipPreservesOldContent)
{
    // 损坏 zip → 解压失败 → 旧目录原样保留，.update 临时目录清理。
    FakePiHttpClient http;
    http.AddRoute("/download/bad.zip", "not-a-real-zip-bytes");

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/bad.zip";

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_APPLY_FAILED);

    // 旧内容未被替换。
    std::ifstream old_interface(project.Dir() / "interface.json");
    std::string   content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
    // 未生成 backup。
    const auto backup_dir =
        std::filesystem::path(project.Dir().string() + "_backup_1.0.0");
    EXPECT_FALSE(std::filesystem::exists(backup_dir));
    // .update 临时目录已清理。
    const auto update_root = project.Dir().parent_path()
                             / (project.Dir().filename().string() + ".update");
    EXPECT_FALSE(std::filesystem::exists(update_root));
}

TEST(PiUpdateService, ApplyUpdateRenameFailurePreservesOldContent)
{
    // 备份/替换 rename 任一步失败 → 旧目录原样保留（不产生半替换态）。
    // 用一个打开的 ifstream 持有 pi_dir 内文件的句柄（Windows sharing
    // violation 使 backup rename 失败）→ ApplyUpdate 返回失败且旧内容完整。
    FakePiHttpClient http;
    http.AddRoute(
        "/download/package.zip",
        BuildTestZip({
            TestZipEntry{"interface.json", "{\"version\":\"1.2.0\"}"},
        }));

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/package.zip";

    // 持有句柄阻止目录 rename（仅 Windows 语义；POSIX 上 rename 目录不受
    // 打开文件影响，但该环境为 Windows/MinGW，共享违规确定发生）。
    std::ifstream lock(project.Dir() / "interface.json", std::ios::binary);

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);
    lock.close();

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_APPLY_FAILED);

    // 旧内容未被替换。
    std::ifstream old_interface(project.Dir() / "interface.json");
    std::string   content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
    // .update 临时目录已清理。
    const auto update_root = project.Dir().parent_path()
                             / (project.Dir().filename().string() + ".update");
    EXPECT_FALSE(std::filesystem::exists(update_root));
}
