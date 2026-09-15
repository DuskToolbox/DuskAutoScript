#include "../src/PiUpdateService.h"
#include "../src/PiHttpClient.h"

#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Utils/PathText.h>
#include <das/_autogen/idl/abi/DasResult.h>
#include <gtest/gtest.h>
#include <openssl/sha.h>
#include <zlib.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
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
            // 模拟流式下载：false → 无 Content-Length（total_bytes
            // 空，阶段态）； chunk_size=0 → 整包一次写入 + 单次回调，非 0 →
            // 按块回调。
            bool          has_content_length = true;
            std::uint64_t chunk_size = 0;
        };

        struct Call
        {
            std::string url;
            int         status = 0;
            std::string body;
            std::string proxy_url;
        };

        void AddRoute(
            std::string   path_fragment,
            std::string   body,
            int           status = 200,
            bool          has_content_length = true,
            std::uint64_t chunk_size = 0)
        {
            routes_.push_back(
                Route{
                    std::move(path_fragment),
                    std::move(body),
                    status,
                    has_content_length,
                    chunk_size});
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

        // 流式下载 mock：命中路由时把 body 写入 out_path（按 chunk_size 分块）
        // 并逐块回调 on_progress；has_content_length=false 时 total_bytes 空。
        DasResult DownloadStream(
            const std::string&           url,
            const std::filesystem::path& out_path,
            const ProgressCallback&      on_progress,
            int&                         out_status,
            const std::string&           proxy_url) override
        {
            calls_.push_back(Call{url, 0, {}, proxy_url});
            if (fail_all_)
            {
                return DAS_E_FAIL;
            }
            for (const auto& route : routes_)
            {
                if (url.find(route.path_fragment) != std::string::npos)
                {
                    out_status = route.status;
                    calls_.back().status = route.status;
                    calls_.back().body = route.body;
                    if (route.status != 200)
                    {
                        return DAS_S_OK;
                    }
                    std::ofstream ofs(
                        out_path,
                        std::ios::binary | std::ios::trunc);
                    if (!ofs)
                    {
                        return DAS_E_FAIL;
                    }
                    std::optional<std::uint64_t> total;
                    if (route.has_content_length)
                    {
                        total = static_cast<std::uint64_t>(route.body.size());
                    }
                    const std::uint64_t body_size =
                        static_cast<std::uint64_t>(route.body.size());
                    if (body_size == 0)
                    {
                        if (on_progress)
                        {
                            on_progress(DownloadProgress{0, total});
                        }
                        return DAS_S_OK;
                    }
                    const std::uint64_t step =
                        route.chunk_size == 0 ? body_size : route.chunk_size;
                    std::uint64_t offset = 0;
                    do
                    {
                        std::uint64_t n = body_size - offset;
                        if (n > step)
                        {
                            n = step;
                        }
                        ofs.write(
                            route.body.data() + offset,
                            static_cast<std::streamsize>(n));
                        offset += n;
                        if (on_progress)
                        {
                            on_progress(DownloadProgress{offset, total});
                        }
                    } while (offset < body_size);
                    return DAS_S_OK;
                }
            }
            out_status = 404;
            calls_.back().status = 404;
            return DAS_S_OK;
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
            dir_ = std::filesystem::temp_directory_path()
                   / Das::Utils::PathFromUtf8(name_);
            Cleanup();
            std::error_code ec;
            std::filesystem::create_directories(dir_, ec);
        }

        ~TempPiProject() { Cleanup(); }

        const std::filesystem::path& Dir() const { return dir_; }

        void WriteInterface(const std::string& json)
        {
            std::ofstream file(
                dir_ / DAS_PATH_LITERAL("interface.json"),
                std::ios::binary);
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
                const auto leaf =
                    Das::Utils::PathToUtf8(entry.path().filename());
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
        std::string   filename;
        std::string   content;
        bool          deflate = false;
        std::uint32_t declared_uncompressed_size = 0;
        std::uint32_t declared_crc = 0;
    };

    // 构造一个合法的 ZIP（stored / deflate），支持故意含 ../ 的穿越条目。
    // declared_uncompressed_size 非 0 时，zip 头中的 uncompressed size 字段
    // 用该值（可伪造远超真实内容大小的声明，用于解压炸弹测试）；
    // declared_crc 非 0 时，local header 与 central directory 的 CRC 字段
    // 都写该声明值（伪造与实际内容不符的 CRC，用于损坏包拒绝测试）。
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
            AppendU16(central_section, 0);  // flags
            AppendU16(central_section, entry.deflate ? 8 : 0);
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

    // sha256（十六进制小写）——与生产 ComputeSha256Hex 同算法（OpenSSL 一次性
    // API），供测试构造「与 zip 本体一致」的声明哈希。
    std::string Sha256Hex(const std::string& data)
    {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(
            reinterpret_cast<const unsigned char*>(data.data()),
            data.size(),
            digest);
        constexpr char HEX_DIGITS[] = "0123456789abcdef";
        std::string    out;
        out.reserve(SHA256_DIGEST_LENGTH * 2);
        for (const unsigned char byte : digest)
        {
            out.push_back(HEX_DIGITS[byte >> 4]);
            out.push_back(HEX_DIGITS[byte & 0xF]);
        }
        return out;
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
    const auto check = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(check.result, DAS_S_OK);
    EXPECT_TRUE(info.has_update);
    EXPECT_EQ(info.version_name, "1.2.0");
    EXPECT_EQ(info.download_url, "https://mirror.example/package.zip");
    EXPECT_TRUE(info.sha256.has_value());

    EXPECT_FALSE(info.error_code.has_value());
    // MirrorChyan won — GitHub was never contacted.
    EXPECT_EQ(http.CountRequestsWith("/repos/owner/repo"), 0);
    EXPECT_EQ(http.CountRequestsWith("/api/resources/rid-1/latest"), 1);
}

// 检查失败原因分类：GitHub API 限流（403）→ 限流文案 key；网络层失败
// （无 HTTP 状态）→ 网络文案 key。分类供 authoring 层选用户可见提示，
// 替代无差别的「检查更新失败」。
TEST(PiUpdateService, CheckUpdateFailureReasonCarriesHttpStatus)
{
    FakePiHttpClient http;
    http.AddRoute("api.github.com", "{}", /*status=*/403);

    TempPiProject project;
    project.WriteInterface(
        MakeInterface("1.0.0", "https://github.com/owner/repo", ""));

    PiUpdateInfo info;
    const auto   check = PiUpdateService::CheckUpdate(
        project.Dir(),
        http,
        info,
        "",
        "",
        "github");

    EXPECT_EQ(check.result, DAS_E_MAAPI_UPDATE_CHECK_FAILED);
    EXPECT_EQ(check.failure_reason_key, "diagnostic.checkUpdateRequestFailed");
}

TEST(PiUpdateService, CheckUpdateFailureReasonClassifiesNetworkFailure)
{
    FakePiHttpClient http;
    http.SetNetworkFailure(true);

    TempPiProject project;
    project.WriteInterface(
        MakeInterface("1.0.0", "https://github.com/owner/repo", ""));

    PiUpdateInfo info;
    const auto   check = PiUpdateService::CheckUpdate(
        project.Dir(),
        http,
        info,
        "",
        "",
        "github");

    EXPECT_EQ(check.result, DAS_E_MAAPI_UPDATE_CHECK_FAILED);
    EXPECT_EQ(check.failure_reason_key, "diagnostic.checkUpdateNetworkFailed");
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
    const auto check = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(check.result, DAS_S_OK);
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
    const auto check = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(check.result, DAS_S_OK);
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
    const auto check = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(check.result, DAS_E_MAAPI_UPDATE_CHECK_FAILED);
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
    const auto check = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(check.result, DAS_S_OK);
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

    constexpr std::string_view test_proxy = "http://127.0.0.1:8899";
    PiUpdateInfo               info;
    const auto                 check =
        PiUpdateService::CheckUpdate(project.Dir(), http, info, test_proxy);

    EXPECT_EQ(check.result, DAS_S_OK);
    EXPECT_TRUE(info.has_update);
    // Every request through the seam carried the proxy URL.
    ASSERT_FALSE(http.Calls().empty());
    for (const auto& call : http.Calls())
    {
        EXPECT_EQ(call.proxy_url, test_proxy);
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
    const auto check = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(check.result, DAS_E_MAAPI_UPDATE_CHECK_FAILED);
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
    const auto check = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(check.result, DAS_S_OK);
    EXPECT_TRUE(info.has_update);
    // GitHub full-package URL adopted, not the incremental mirror link.
    EXPECT_EQ(
        info.download_url,
        "https://github.com/owner/repo/releases/download/v1.2.0/"
        "test-project-win-x86_64-v1.2.0.zip");
}

TEST(PiUpdateService, CheckUpdateMirrorNoUrlGithubFailsKeepsUpdateInfo)
{
    // 无 CDK 匿名查询：MirrorChyan code=0 查到新版本但 data.url 空 → 降级
    // GitHub 补链接；GitHub 也不可用（无路由 → 404）→ 版本信息仍有效，
    // CheckUpdate 返回成功（download_url 留空），不当成检查失败。
    FakePiHttpClient http;
    http.AddRoute(
        "/api/resources/rid-nocdk/latest",
        MirrorResponse(
            "1.2.0",
            /*code=*/0,
            /*url=*/"",
            /*sha256=*/"",
            /*update_type=*/""));

    TempPiProject project;
    project.WriteInterface(
        MakeInterface("1.0.0", "https://github.com/owner/repo", "rid-nocdk"));

    PiUpdateInfo info;
    const auto check = PiUpdateService::CheckUpdate(project.Dir(), http, info);

    EXPECT_EQ(check.result, DAS_S_OK);
    EXPECT_TRUE(info.has_update);
    EXPECT_EQ(info.version_name, "1.2.0");
    EXPECT_TRUE(info.download_url.empty());
    EXPECT_FALSE(info.error_code.has_value());
    // GitHub 兜底确实被尝试过。
    EXPECT_EQ(http.CountRequestsWith("/repos/owner/repo"), 1);
}

TEST(PiUpdateService, CheckUpdateGithubChannelFailureKeepsVersionInfo)
{
    // updateChannel=github 单通道：GitHub 拿到 release 但 asset 匹配失败 +
    // 直链 HEAD 404 → 检查结果仍成立（版本信息保留、无下载链接）。
    FakePiHttpClient http;
    // asset 名不带 os/arch 别名 → MatchGithubAsset 无匹配。
    http.AddRoute(
        "/repos/owner/repo/releases/latest",
        GithubResponse("v1.2.0", "source-code-only.tar.gz"));

    TempPiProject project;
    project.WriteInterface(
        MakeInterface("1.0.0", "https://github.com/owner/repo", "rid-x"));

    PiUpdateInfo info;
    const auto   check = PiUpdateService::CheckUpdate(
        project.Dir(),
        http,
        info,
        /*proxy_url=*/{},
        /*mirrorchyan_cdk=*/{},
        /*update_channel=*/"github");

    EXPECT_EQ(check.result, DAS_S_OK);
    EXPECT_TRUE(info.has_update);
    EXPECT_EQ(info.version_name, "v1.2.0");
    EXPECT_TRUE(info.download_url.empty());
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
    std::ifstream new_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(new_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "{\"version\":\"1.2.0\"}");
    EXPECT_TRUE(
        std::filesystem::exists(
            project.Dir() / DAS_PATH_LITERAL("config.json")));
    EXPECT_TRUE(
        std::filesystem::exists(
            project.Dir() / DAS_PATH_LITERAL("assets/image.png")));

    // 备份目录存在（{pi_dir}_backup_{oldVersion}）。
    auto backup_dir = project.Dir();
    backup_dir += DAS_PATH_LITERAL("_backup_1.0.0");
    EXPECT_TRUE(std::filesystem::is_directory(backup_dir));
    EXPECT_TRUE(
        std::filesystem::exists(
            backup_dir / DAS_PATH_LITERAL("interface.json")));

    // .update 临时目录已清理。
    auto update_dir_name = project.Dir().filename();
    update_dir_name += DAS_PATH_LITERAL(".update");
    const auto update_root = project.Dir().parent_path() / update_dir_name;
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
    EXPECT_TRUE(
        std::filesystem::exists(
            project.Dir() / DAS_PATH_LITERAL("interface.json")));
    std::ifstream old_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
    EXPECT_FALSE(
        std::filesystem::exists(
            std::filesystem::temp_directory_path()
            / DAS_PATH_LITERAL("evil.txt")));
    // .update 临时目录已清理。
    auto update_dir_name = project.Dir().filename();
    update_dir_name += DAS_PATH_LITERAL(".update");
    const auto update_root = project.Dir().parent_path() / update_dir_name;
    EXPECT_FALSE(std::filesystem::exists(update_root));
}

TEST(PiUpdateService, ApplyUpdateRejectsDecompressionBomb)
{
    // WR-02: 条目 central directory 声称解压 1GB（远超市面全量包体积）→
    // 解压炸弹防护必须在任何 4GB/1GB resize 前拒绝，旧内容不被替换。
    FakePiHttpClient http;
    http.AddRoute(
        "/download/bomb.zip",
        BuildTestZip({
            TestZipEntry{
                "huge.bin",
                "tiny-compressed-data",
                /*deflate=*/true,
                /*declared_uncompressed_size=*/1024u * 1024u * 1024u},
            TestZipEntry{"interface.json", "{\"version\":\"1.2.0\"}"},
        }));

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/bomb.zip";

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_APPLY_FAILED);

    // 旧内容未被替换（解压阶段失败，pi_dir 未动）。
    std::ifstream old_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
    // .update 临时目录已清理。
    auto update_dir_name = project.Dir().filename();
    update_dir_name += DAS_PATH_LITERAL(".update");
    const auto update_root = project.Dir().parent_path() / update_dir_name;
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
    std::ifstream old_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
    // 未生成 backup。
    auto backup_dir = project.Dir();
    backup_dir += DAS_PATH_LITERAL("_backup_1.0.0");
    EXPECT_FALSE(std::filesystem::exists(backup_dir));
    // .update 临时目录已清理。
    auto update_dir_name = project.Dir().filename();
    update_dir_name += DAS_PATH_LITERAL(".update");
    const auto update_root = project.Dir().parent_path() / update_dir_name;
    EXPECT_FALSE(std::filesystem::exists(update_root));
}

TEST(PiUpdateService, ApplyUpdateRejectsStoredEntryWithCorruptCrc)
{
    // 声明 CRC 与实际内容不符（stored 条目）→ 解压路径的 CRC 校验必须
    // 拒绝该条目，整体失败且旧内容保留。
    FakePiHttpClient http;
    http.AddRoute(
        "/download/corrupt-stored.zip",
        BuildTestZip({
            TestZipEntry{
                "interface.json",
                "{\"version\":\"1.2.0\"}",
                /*deflate=*/false,
                /*declared_uncompressed_size=*/0,
                /*declared_crc=*/0xDEADBEEF},
        }));

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/corrupt-stored.zip";

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_APPLY_FAILED);

    // 旧内容未被替换（解压阶段失败，pi_dir 未动）。
    std::ifstream old_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
    // 未生成 backup。
    auto backup_dir = project.Dir();
    backup_dir += DAS_PATH_LITERAL("_backup_1.0.0");
    EXPECT_FALSE(std::filesystem::exists(backup_dir));
    // .update 临时目录（含输出目录）已清理。
    auto update_dir_name = project.Dir().filename();
    update_dir_name += DAS_PATH_LITERAL(".update");
    const auto update_root = project.Dir().parent_path() / update_dir_name;
    EXPECT_FALSE(std::filesystem::exists(update_root));
}

TEST(PiUpdateService, ApplyUpdateRejectsDeflateEntryWithCorruptCrc)
{
    // 声明 CRC 与实际内容不符（deflate 条目）→ 解压路径的 CRC 校验必须
    // 拒绝该条目，整体失败且旧内容保留。
    FakePiHttpClient http;
    http.AddRoute(
        "/download/corrupt-deflate.zip",
        BuildTestZip({
            TestZipEntry{
                "interface.json",
                "{\"version\":\"1.2.0\"}",
                /*deflate=*/true,
                /*declared_uncompressed_size=*/0,
                /*declared_crc=*/0xDEADBEEF},
            TestZipEntry{"config.json", "{\"speed\":10}"},
        }));

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/corrupt-deflate.zip";

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_APPLY_FAILED);

    // 旧内容未被替换。
    std::ifstream old_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
    // 未生成 backup。
    auto backup_dir = project.Dir();
    backup_dir += DAS_PATH_LITERAL("_backup_1.0.0");
    EXPECT_FALSE(std::filesystem::exists(backup_dir));
    // .update 临时目录（含输出目录）已清理。
    auto update_dir_name = project.Dir().filename();
    update_dir_name += DAS_PATH_LITERAL(".update");
    const auto update_root = project.Dir().parent_path() / update_dir_name;
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
    std::ifstream lock(
        project.Dir() / DAS_PATH_LITERAL("interface.json"),
        std::ios::binary);

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);
    lock.close();

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_APPLY_FAILED);

    // 旧内容未被替换。
    std::ifstream old_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
    // .update 临时目录已清理。
    auto update_dir_name = project.Dir().filename();
    update_dir_name += DAS_PATH_LITERAL(".update");
    const auto update_root = project.Dir().parent_path() / update_dir_name;
    EXPECT_FALSE(std::filesystem::exists(update_root));
}

// ---------------------------------------------------------------------
// Task 3：resource.hash 校验（警告不硬失败）+ 被锁文件 .pending 暂存
// ---------------------------------------------------------------------

TEST(PiUpdateService, ApplyUpdateSha256MismatchRejectsPackage)
{
    // 来源声明 sha256 与 zip 本体实际哈希不符 → 硬拒（-10008），不进解压，
    // 旧内容保留，.update 临时目录清理。
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
    info.sha256 =
        "0000000000000000000000000000000000000000000000000000000000000000";

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_APPLY_FAILED);

    // 旧内容未被替换（sha256 校验失败于解压前，pi_dir 未动）。
    std::ifstream old_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
    // .update 临时目录已清理。
    auto update_dir_name = project.Dir().filename();
    update_dir_name += DAS_PATH_LITERAL(".update");
    const auto update_root = project.Dir().parent_path() / update_dir_name;
    EXPECT_FALSE(std::filesystem::exists(update_root));
}

TEST(PiUpdateService, ApplyUpdateSha256MatchSucceeds)
{
    // 来源声明 sha256 与 zip 本体一致 → 校验通过，更新成功落地。
    const std::string zip_data = BuildTestZip({
        TestZipEntry{"interface.json", "{\"version\":\"1.2.0\"}"},
    });
    FakePiHttpClient  http;
    http.AddRoute("/download/package.zip", zip_data);

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/package.zip";
    info.sha256 = Sha256Hex(zip_data);

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);

    EXPECT_EQ(result, DAS_S_OK);
    std::ifstream new_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(new_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "{\"version\":\"1.2.0\"}");
}

TEST(PiUpdateService, ApplyUpdateWithoutSha256SkipsVerification)
{
    // GitHub 通道不提供包哈希（sha256 空）→ 跳过校验，更新照常成功。
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
    // info.sha256 保持空 optional。

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);

    EXPECT_EQ(result, DAS_S_OK);
    std::ifstream new_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(new_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "{\"version\":\"1.2.0\"}");
}

TEST(PiUpdateService, ApplyUpdateCancelDuringExtractionReturnsCancelled)
{
    // 取消观察缝：解压循环条目开头命中 → 专用取消码，旧内容保留，.update
    // 临时目录清理。cancel_check 首次调用 false（首条目照常解压）、之后
    // true（第二条目开头中止）。
    FakePiHttpClient http;
    http.AddRoute(
        "/download/package.zip",
        BuildTestZip({
            TestZipEntry{"interface.json", "{\"version\":\"1.2.0\"}"},
            TestZipEntry{"config.json", "{\"speed\":10}"},
            TestZipEntry{"assets/image.png", "fake-image-bytes"},
        }));

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/package.zip";

    int        cancel_call_count = 0;
    const auto result = PiUpdateService::ApplyUpdate(
        project.Dir(),
        info,
        http,
        /*proxy_url=*/{},
        /*mirrorchyan_cdk=*/{},
        /*progress_reporter=*/{},
        /*cancel_check=*/[&cancel_call_count]
        { return ++cancel_call_count > 1; });

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_CANCELLED);

    // 旧内容未被替换（取消发生在解压阶段，pi_dir 未动）。
    std::ifstream old_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
    // .update 临时目录已清理。
    auto update_dir_name = project.Dir().filename();
    update_dir_name += DAS_PATH_LITERAL(".update");
    const auto update_root = project.Dir().parent_path() / update_dir_name;
    EXPECT_FALSE(std::filesystem::exists(update_root));
}

TEST(PiUpdateService, ApplyUpdateCancelDuringReplaceReturnsCancelled)
{
    // 替换循环取消：被锁文件触发逐文件降级路径后，首文件开头命中取消 →
    // 专用取消码，pi_dir 保留旧内容。
    FakePiHttpClient http;
    http.AddRoute(
        "/download/package.zip",
        BuildTestZip({
            TestZipEntry{"interface.json", "{\"version\":\"1.2.0\"}"},
            TestZipEntry{"MaaFramework.dll", "new-dll-bytes"},
        }));

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));
    {
        std::ofstream locked(
            project.Dir() / DAS_PATH_LITERAL("MaaFramework.dll"),
            std::ios::binary);
        locked << "old-dll-bytes";
        locked.flush();
    }
    // 持有句柄阻止目录整体 rename → 逐文件替换降级路径。
    std::ifstream lock(
        project.Dir() / DAS_PATH_LITERAL("MaaFramework.dll"),
        std::ios::binary);

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/package.zip";

    const auto result = PiUpdateService::ApplyUpdate(
        project.Dir(),
        info,
        http,
        /*proxy_url=*/{},
        /*mirrorchyan_cdk=*/{},
        /*progress_reporter=*/{},
        /*cancel_check=*/[] { return true; });
    lock.close();

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_CANCELLED);

    // interface.json 未被替换（首文件处理前即中止）。
    std::ifstream old_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(old_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
}

TEST(PiUpdateService, ApplyUpdateLockedFileStagedToPending)
{
    // 项目目录内一个文件被占用（Windows sharing violation）→ 整体 rename
    // 失败 → 降级逐文件替换：该文件新版本进 {pi_dir}/.pending/，其余正常替换
    // （D-89-06），更新仍成功。
    FakePiHttpClient http;
    http.AddRoute(
        "/download/package.zip",
        BuildTestZip({
            TestZipEntry{"interface.json", "{\"version\":\"1.2.0\"}"},
            TestZipEntry{"MaaFramework.dll", "new-dll-bytes"},
        }));

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));
    {
        // 预创建占用文件（持有句柄阻止删除/rename）。
        std::ofstream locked(
            project.Dir() / DAS_PATH_LITERAL("MaaFramework.dll"),
            std::ios::binary);
        locked << "old-dll-bytes";
        locked.flush();
    }
    // 用只读属性 + 目录整体 rename 依赖的打开句柄来稳定触发 sharing
    // violation：先以可写句柄持有，保证目录 rename 失败。
    std::ifstream lock(
        project.Dir() / DAS_PATH_LITERAL("MaaFramework.dll"),
        std::ios::binary);

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/package.zip";

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);
    lock.close();

    EXPECT_EQ(result, DAS_S_OK);
    // interface.json 已替换。
    std::ifstream new_interface(
        project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string content(
        (std::istreambuf_iterator<char>(new_interface)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "{\"version\":\"1.2.0\"}");
    // 被锁 DLL 新版本暂存到 .pending/。
    const auto pending = project.Dir() / DAS_PATH_LITERAL(".pending")
                         / DAS_PATH_LITERAL("MaaFramework.dll");
    EXPECT_TRUE(std::filesystem::exists(pending));
}

TEST(PiUpdateService, ApplyUpdateInterfaceJsonLockedPreservesAllFiles)
{
    // WR-03: 逐文件替换降级路径下 interface.json 被锁 → -10008 且其他文件
    // 未被触碰（interface.json 优先处理），旧目录保持完整，满足头文件
    // "-10008 时旧目录已回滚"契约。
    FakePiHttpClient http;
    http.AddRoute(
        "/download/package.zip",
        BuildTestZip({
            TestZipEntry{"interface.json", "{\"version\":\"1.2.0\"}"},
            TestZipEntry{"config.json", "{\"speed\":99}"},
        }));

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));
    {
        std::ofstream old_config(
            project.Dir() / DAS_PATH_LITERAL("config.json"),
            std::ios::binary);
        old_config << "{\"speed\":1}";
        old_config.flush();
    }
    // 锁住 interface.json → 整体目录 rename 失败，逐文件替换第一步即失败。
    std::ifstream lock(
        project.Dir() / DAS_PATH_LITERAL("interface.json"),
        std::ios::binary);

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/package.zip";

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);
    lock.close();

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_APPLY_FAILED);

    // interface.json 与 config.json 都保持旧内容（interface.json 最先处理，
    // 失败时无其他文件被替换）。
    std::ifstream iface(project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string   iface_content(
        (std::istreambuf_iterator<char>(iface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(iface_content.find("1.0.0"), std::string::npos);
    std::ifstream cfg(project.Dir() / DAS_PATH_LITERAL("config.json"));
    std::string   cfg_content(
        (std::istreambuf_iterator<char>(cfg)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(cfg_content.find("{\"speed\":1}"), std::string::npos);
    // 未生成 backup 目录（整体 rename 失败分支不回退到 backup rename）。
    auto backup_dir = project.Dir();
    backup_dir += DAS_PATH_LITERAL("_backup_1.0.0");
    EXPECT_FALSE(std::filesystem::exists(backup_dir));
}

// ---------------------------------------------------------------------
// Task 4：MaaVersion 硬链调用（版本显示数据源，D-89-07 修订）
// ---------------------------------------------------------------------

TEST(PiUpdateService, MaaVersionResolvedFromHardlink)
{
    // MaaVersion() 是 MaaFramework 通用跨平台 API，由 AuthoringProjector
    // 硬链直接调用（与 PiCompiler QueryMaaFrameworkVersion 同源），不经动态
    // 符号解析。生产语义：硬链返回非空版本串（MaaFW 静态字符串，立即拷贝）。
    // 测试构建（DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY）不链接 MaaFramework，
    // MaaVersion() 不可用，此测试仅在生产构建下有意义。
#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
    const std::string version_text{MaaVersion()};
    EXPECT_FALSE(version_text.empty());
#else
    SUCCEED() << "MaaVersion hard-link unavailable in test build — "
                 "version display is production-only";
#endif
}

// ---------------------------------------------------------------------
// 流式下载进度上报：阶段序列 + 有/无 total + 失败路径
// ---------------------------------------------------------------------

TEST(PiUpdateService, ApplyUpdateReportsDownloadExtractReplaceStages)
{
    // 有 Content-Length 的 mock 下载（分块回调）→ ApplyUpdate 触发 download
    // 阶段回调带 bytes/total，随后 Extract / Replace 各一次阶段事件。
    FakePiHttpClient http;
    http.AddRoute(
        "/download/package.zip",
        BuildTestZip(
            {TestZipEntry{"interface.json", "{\"version\":\"1.2.0\"}"}}),
        200,
        /*has_content_length=*/true,
        /*chunk_size=*/4);

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/package.zip";

    std::vector<UpdateProgress> progress;
    const auto                  result = PiUpdateService::ApplyUpdate(
        project.Dir(),
        info,
        http,
        /*proxy_url=*/{},
        /*mirrorchyan_cdk=*/{},
        [&](const UpdateProgress& p) { progress.push_back(p); });

    EXPECT_EQ(result, DAS_S_OK);

    ASSERT_FALSE(progress.empty());
    // 下载阶段至少一次，且最终字节对 = 包大小。
    std::optional<UpdateProgress> download_stage;
    std::optional<std::size_t>    extract_index;
    std::optional<std::size_t>    replace_index;
    for (std::size_t i = 0; i < progress.size(); ++i)
    {
        const auto& p = progress[i];
        if (p.stage == UpdateStage::Download)
        {
            download_stage = p;
        }
        else if (p.stage == UpdateStage::Extract)
        {
            extract_index = i;
        }
        else if (p.stage == UpdateStage::Replace)
        {
            replace_index = i;
        }
    }
    ASSERT_TRUE(download_stage.has_value());
    EXPECT_GT(download_stage->bytes_received, 0u);
    ASSERT_TRUE(download_stage->total_bytes.has_value());
    EXPECT_EQ(*download_stage->total_bytes, download_stage->bytes_received);
    // UpdateProgress 携带速率与已用时间字段（供投影层消费）。
    EXPECT_GE(download_stage->rate_mbps, 0.0);
    EXPECT_GE(download_stage->elapsed.count(), 0);
    // 阶段序列 Download → Extract → Replace。
    ASSERT_TRUE(extract_index.has_value());
    ASSERT_TRUE(replace_index.has_value());
    EXPECT_LT(*extract_index, *replace_index);
}

TEST(PiUpdateService, ApplyUpdateWithoutContentLengthReportsStageOnly)
{
    // 无 Content-Length（has_content_length=false）→ download 阶段回调
    // total 为空（阶段态，不估算字节/百分比）。
    FakePiHttpClient http;
    http.AddRoute(
        "/download/package.zip",
        BuildTestZip(
            {TestZipEntry{"interface.json", "{\"version\":\"1.2.0\"}"}}),
        200,
        /*has_content_length=*/false);

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/package.zip";

    std::vector<UpdateProgress> progress;
    const auto                  result = PiUpdateService::ApplyUpdate(
        project.Dir(),
        info,
        http,
        {},
        {},
        [&](const UpdateProgress& p) { progress.push_back(p); });

    EXPECT_EQ(result, DAS_S_OK);

    bool saw_download = false;
    bool saw_extract = false;
    bool saw_replace = false;
    for (const auto& p : progress)
    {
        if (p.stage == UpdateStage::Download)
        {
            saw_download = true;
            EXPECT_FALSE(p.total_bytes.has_value());
        }
        else if (p.stage == UpdateStage::Extract)
        {
            saw_extract = true;
        }
        else if (p.stage == UpdateStage::Replace)
        {
            saw_replace = true;
        }
    }
    EXPECT_TRUE(saw_download);
    EXPECT_TRUE(saw_extract);
    EXPECT_TRUE(saw_replace);
}

TEST(PiUpdateService, ApplyUpdateDownloadFailurePreservesOldContent)
{
    // 流式下载失败（mock 返回非 200）→ ApplyUpdate 返回 -10008，且不落坏
    // zip（.update 临时目录清理），旧内容保留。
    FakePiHttpClient http;
    http.AddRoute("/download/package.zip", "not-a-zip", /*status=*/500);

    TempPiProject project;
    project.WriteInterface(MakeInterface("1.0.0", "", ""));

    PiUpdateInfo info;
    info.version_name = "1.2.0";
    info.download_url = "https://example.com/download/package.zip";

    const auto result = PiUpdateService::ApplyUpdate(project.Dir(), info, http);

    EXPECT_EQ(result, DAS_E_MAAPI_UPDATE_APPLY_FAILED);

    // 无坏 zip 残留。
    auto update_dir_name = project.Dir().filename();
    update_dir_name += DAS_PATH_LITERAL(".update");
    const auto update_root = project.Dir().parent_path() / update_dir_name;
    EXPECT_FALSE(std::filesystem::exists(update_root));
    // 旧内容保留。
    std::ifstream iface(project.Dir() / DAS_PATH_LITERAL("interface.json"));
    std::string   content(
        (std::istreambuf_iterator<char>(iface)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("1.0.0"), std::string::npos);
}

// ---------------------------------------------------------------------
// 下载速率计算 DownloadRateTracker：公式 + 除零保护 + 滑动窗口平滑
// ---------------------------------------------------------------------

TEST(PiUpdateService, DownloadRateTrackerComputesMbpsOverWindow)
{
    // 注入固定时钟：t0 收到 0 字节，t0+1s 收到 2.4MB → 速率 = 2.4 MB/s。
    using Clock = DownloadRateTracker::Clock;
    auto                current = Clock::now();
    DownloadRateTracker tracker([&] { return current; });

    tracker.OnChunk(0);
    current += std::chrono::seconds(1);
    tracker.OnChunk(static_cast<std::uint64_t>(2.4 * 1024 * 1024));

    EXPECT_NEAR(tracker.CurrentRateMbps(), 2.4, 0.01);
}

TEST(PiUpdateService, DownloadRateTrackerZeroWindowReturnsZero)
{
    // 同一时刻两个采样 → 时间窗 0 → 速率 0（不除零）。
    using Clock = DownloadRateTracker::Clock;
    auto                current = Clock::now();
    DownloadRateTracker tracker([&] { return current; });

    tracker.OnChunk(0);
    tracker.OnChunk(1024 * 1024);
    EXPECT_EQ(tracker.CurrentRateMbps(), 0.0);

    // 单采样 → 0。
    DownloadRateTracker single([&] { return current; });
    single.OnChunk(100);
    EXPECT_EQ(single.CurrentRateMbps(), 0.0);
}

TEST(PiUpdateService, DownloadRateTrackerSlidingWindowAverages)
{
    // 每秒均匀 +1MB，共 4 秒；滑动 2s 窗口内速率应稳定在 ~1 MB/s。
    using Clock = DownloadRateTracker::Clock;
    auto                current = Clock::now();
    DownloadRateTracker tracker([&] { return current; });

    for (int i = 0; i <= 4; ++i)
    {
        tracker.OnChunk(static_cast<std::uint64_t>(i) * 1024u * 1024u);
        current += std::chrono::seconds(1);
    }
    // 窗口内 (4MB - 2MB) / 2s ≈ 1 MB/s（不随单点跳动）。
    EXPECT_NEAR(tracker.CurrentRateMbps(), 1.0, 0.01);
}
