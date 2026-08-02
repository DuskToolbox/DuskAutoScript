#include "../src/PiUpdateService.h"
#include "../src/PiHttpClient.h"

#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/_autogen/idl/abi/DasResult.h>
#include <gtest/gtest.h>

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

    // Temp project dir with interface.json.
    class TempPiProject
    {
    public:
        TempPiProject()
        {
            dir_ = std::filesystem::temp_directory_path()
                   / "PiUpdate_Test_Project";
            std::error_code ec;
            std::filesystem::remove_all(dir_, ec);
            std::filesystem::create_directories(dir_, ec);
        }

        ~TempPiProject()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir_, ec);
        }

        const std::filesystem::path& Dir() const { return dir_; }

        void WriteInterface(const std::string& json)
        {
            std::ofstream file(dir_ / "interface.json", std::ios::binary);
            file << json;
        }

    private:
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
