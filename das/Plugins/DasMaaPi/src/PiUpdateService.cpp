#include "PiUpdateService.h"
#include "PiSemver.h"

#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/DasJsonCore.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        // MirrorChyan 检查 / GitHub 兜底 / 直接下载拼接的端到端默认值
        // （MXU updateService.ts:74-77 MIRRORCHYAN_API_BASES 对齐）。
        constexpr std::string_view kMirrorchyanBasePrimary =
            "https://mirrorchyan.com/api/resources";
        constexpr std::string_view kMirrorchyanBaseBackup =
            "https://mirrorchyan.net/api/resources";
        constexpr std::string_view kGithubApiBase = "https://api.github.com";
        constexpr std::string_view kGithubWebBase = "https://github.com";

        std::string Lower(std::string s)
        {
            std::transform(
                s.begin(),
                s.end(),
                s.begin(),
                [](unsigned char c)
                { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // 查询参数最小 percent-encode（保留 RFC 3986 unreserved +
        // 版本常见字符）。
        std::string UrlEncode(std::string_view value)
        {
            constexpr std::string_view kHex = "0123456789ABCDEF";
            std::string                out;
            for (const unsigned char c : value)
            {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '-' || c == '_'
                    || c == '.' || c == '~')
                {
                    out.push_back(static_cast<char>(c));
                }
                else
                {
                    out.push_back('%');
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0xF]);
                }
            }
            return out;
        }

        void LogError(const std::string& message)
        {
            DAS_LOG_ERROR(message.c_str());
        }

        void LogWarning(const std::string& message)
        {
            DAS_LOG_WARNING(message.c_str());
        }

        // interface.json 顶层元数据（github / mirrorchyan_rid / version）。
        struct InterfaceMetadata
        {
            std::string github;
            std::string mirrorchyan_rid;
            std::string version;
            std::string name;
        };

        template <typename TObject>
        std::optional<std::string> ReadStringField(
            const TObject&   obj,
            std::string_view key)
        {
            if (!obj.contains(key) || !obj[key].is_string())
            {
                return std::nullopt;
            }
            return std::string(obj[key].as_string().value_or(""));
        }

        template <typename TObject>
        auto ReadObjectField(const TObject& obj, std::string_view key)
        {
            if (!obj.contains(key))
            {
                return decltype(obj[key].as_object()){};
            }
            return obj[key].as_object();
        }

        // 复用 PiParser 解析 interface.json；github / mirrorchyan_rid 从顶层
        // 原始 JSON 读取（PiCatalog 只解析 version，须顶层原始读，D-89-02）。
        DasResult ReadInterfaceMetadata(
            const std::filesystem::path& pi_dir,
            InterfaceMetadata&           meta)
        {
            const auto parsed = ParseProjectInterface(
                {.interface_path = pi_dir / "interface.json"});
            if (!parsed.ok)
            {
                LogError(
                    "PiUpdateService: interface.json parse failed: "
                    + std::filesystem::absolute(pi_dir / "interface.json")
                          .string());
                return DAS_E_MAAPI_UPDATE_CHECK_FAILED;
            }
            const auto& catalog = parsed.catalog;
            meta.version = catalog.version;
            meta.name = catalog.name;
            if (meta.version.empty())
            {
                LogError(
                    "PiUpdateService: interface.json missing 'version' field");
                return DAS_E_MAAPI_UPDATE_CHECK_FAILED;
            }

            const auto raw =
                Das::Utils::ParseYyjsonFromString(catalog.raw.raw_json);
            if (!raw)
            {
                LogError("PiUpdateService: interface.json raw json unreadable");
                return DAS_E_MAAPI_UPDATE_CHECK_FAILED;
            }
            auto obj = raw->as_object();
            if (!obj)
            {
                LogError("PiUpdateService: interface.json root not an object");
                return DAS_E_MAAPI_UPDATE_CHECK_FAILED;
            }
            meta.github = ReadStringField(*obj, "github").value_or("");
            meta.mirrorchyan_rid =
                ReadStringField(*obj, "mirrorchyan_rid").value_or("");
            return DAS_S_OK;
        }

        // MirrorChyan 响应结构（MXU MirrorChyanApiResponse）。
        struct MirrorChyanData
        {
            bool        has_data = false;
            int64_t     code = -1;
            std::string msg;
            std::string version_name;
            std::string url;
            std::string sha256;
            std::string release_note;
            std::string update_type;
        };

        std::optional<MirrorChyanData> ParseMirrorChyanResponse(
            const std::string& body)
        {
            const auto parsed = Das::Utils::ParseYyjsonFromString(body);
            if (!parsed)
            {
                return std::nullopt;
            }
            auto obj = parsed->as_object();
            if (!obj)
            {
                return std::nullopt;
            }
            MirrorChyanData data;
            // as_sint() handles both sint and uint (yyjson stores non-negative
            // integers as uint, so is_sint() would be false for e.g. code==0).
            data.code = (*obj).contains("code")
                            ? (*obj)["code"].as_sint().value_or(-1)
                            : -1;
            data.msg = ReadStringField(*obj, "msg").value_or("");
            if (auto d = ReadObjectField(*obj, "data"))
            {
                data.has_data = true;
                data.version_name =
                    ReadStringField(*d, "version_name").value_or("");
                data.url = ReadStringField(*d, "url").value_or("");
                data.sha256 = ReadStringField(*d, "sha256").value_or("");
                data.release_note =
                    ReadStringField(*d, "release_note").value_or("");
                data.update_type =
                    ReadStringField(*d, "update_type").value_or("full");
            }
            return data;
        }

        // GitHub Release 结构（MXU GitHubRelease / GitHubAsset）。
        struct GithubAsset
        {
            std::string name;
            int64_t     size = 0;
            std::string browser_download_url;
        };

        struct GithubRelease
        {
            std::string              tag_name;
            std::string              body;
            std::vector<GithubAsset> assets;
        };

        std::optional<GithubRelease> ParseGithubRelease(const std::string& body)
        {
            const auto parsed = Das::Utils::ParseYyjsonFromString(body);
            if (!parsed)
            {
                return std::nullopt;
            }
            auto obj = parsed->as_object();
            if (!obj)
            {
                return std::nullopt;
            }
            GithubRelease release;
            release.tag_name = ReadStringField(*obj, "tag_name").value_or("");
            release.body = ReadStringField(*obj, "body").value_or("");
            if (!(*obj).contains("assets"))
            {
                return release;
            }
            auto assets = (*obj)["assets"].as_array();
            if (!assets)
            {
                return release;
            }
            for (auto it = assets->begin(); it != assets->end(); ++it)
            {
                auto asset_obj = it->as_object();
                if (!asset_obj)
                {
                    continue;
                }
                GithubAsset asset;
                asset.name = ReadStringField(*asset_obj, "name").value_or("");
                asset.browser_download_url =
                    ReadStringField(*asset_obj, "browser_download_url")
                        .value_or("");
                if ((*asset_obj).contains("size"))
                {
                    asset.size = (*asset_obj)["size"].as_sint().value_or(0);
                }
                release.assets.push_back(std::move(asset));
            }
            return release;
        }

        // 解析 github 字段（https://github.com/owner/repo）→ owner/repo。
        bool ParseGithubOwnerRepo(
            std::string_view github,
            std::string&     owner,
            std::string&     repo)
        {
            const auto pos = github.find("github.com/");
            if (pos == std::string_view::npos)
            {
                return false;
            }
            std::string_view rest = github.substr(pos + 11);
            if (rest.empty())
            {
                return false;
            }
            if (rest.back() == '/')
            {
                rest.remove_suffix(1);
            }
            const auto slash = rest.find('/');
            if (slash == std::string_view::npos || slash == 0
                || slash == rest.size() - 1)
            {
                return false;
            }
            owner = std::string(rest.substr(0, slash));
            repo = std::string(rest.substr(slash + 1));
            return !owner.empty() && !repo.empty();
        }

        // 按 OS/arch 别名匹配 GitHub asset（MXU matchGitHubAsset 语义）：
        // osAliases × archAliases 取候选，优先含项目名，其次体积最大。
        std::string MatchGithubAsset(
            const std::vector<GithubAsset>& assets,
            const std::string&              project_name_lower,
            const std::string&              os,
            const std::string&              arch)
        {
            std::vector<std::string> os_aliases;
            std::vector<std::string> arch_aliases;
            if (os == "windows")
            {
                os_aliases = {"win", "windows", "win32", "win64"};
            }
            else if (os == "darwin")
            {
                os_aliases = {"macos", "mac", "darwin", "osx"};
            }
            else
            {
                os_aliases = {"linux"};
            }
            if (arch == "x86_64")
            {
                arch_aliases = {"x86_64", "x64", "amd64", "x86-64"};
            }
            else if (arch == "aarch64")
            {
                arch_aliases = {"aarch64", "arm64"};
            }
            else
            {
                arch_aliases = {arch};
            }

            std::vector<const GithubAsset*> candidates;
            for (const auto& asset : assets)
            {
                const auto name_lower = Lower(asset.name);
                const bool os_match = std::any_of(
                    os_aliases.begin(),
                    os_aliases.end(),
                    [&](const std::string& alias)
                    { return name_lower.find(alias) != std::string::npos; });
                const bool arch_match = std::any_of(
                    arch_aliases.begin(),
                    arch_aliases.end(),
                    [&](const std::string& alias)
                    { return name_lower.find(alias) != std::string::npos; });
                if (os_match && arch_match)
                {
                    candidates.push_back(&asset);
                }
            }
            if (candidates.empty())
            {
                return {};
            }
            // 优先含项目名（MXU 的 -mxu 偏好对 DAS 不适用，用项目名等价）。
            if (!project_name_lower.empty())
            {
                for (const auto* candidate : candidates)
                {
                    if (Lower(candidate->name).find(project_name_lower)
                        != std::string::npos)
                    {
                        return candidate->browser_download_url;
                    }
                }
            }
            const GithubAsset* best = candidates.front();
            for (const auto* candidate : candidates)
            {
                if (candidate->size > best->size)
                {
                    best = candidate;
                }
            }
            return best->browser_download_url;
        }

        // 直接下载链接兜底（MXU buildDirectDownloadUrl）：{projectName}-{os}
        // -{arch}-v{version}.zip + HEAD 探测。
        std::string BuildDirectDownloadUrl(
            std::string_view         owner,
            std::string_view         repo,
            const InterfaceMetadata& meta,
            std::string_view         version,
            std::string_view         os,
            std::string_view         arch,
            std::string_view         web_base)
        {
            const std::string version_tag =
                !version.empty() && version.front() != 'v'
                    ? "v" + std::string(version)
                    : std::string(version);
            const std::string os_tag =
                os == "windows" ? "win"
                                : (os == "darwin" ? "macos" : std::string(os));
            const std::string arch_tag = arch == "x86_64" ? "x86_64"
                                         : arch == "aarch64"
                                             ? "aarch64"
                                             : std::string(arch);
            const std::string filename = meta.name + "-" + os_tag + "-"
                                         + arch_tag + "-" + version_tag
                                         + ".zip";
            return std::string(web_base) + "/" + std::string(owner) + "/"
                   + std::string(repo) + "/releases/download/" + version_tag
                   + "/" + filename;
        }

        DasResult CheckMirrorChyan(
            const InterfaceMetadata& meta,
            PiHttpClient&            http,
            const std::string&       proxy,
            PiUpdateInfo&            out)
        {
            const std::vector<std::string> bases{
                std::string(kMirrorchyanBasePrimary),
                std::string(kMirrorchyanBaseBackup)};
            for (const auto& base : bases)
            {
                const std::string url =
                    base + "/" + meta.mirrorchyan_rid
                    + "/latest?current_version=" + UrlEncode(meta.version)
                    + "&user_agent=DasMaaPi&os=windows"
                      "&arch=x86_64&channel=stable";
                std::string body;
                int         status = 0;
                const auto  result = http.Get(url, body, status, proxy);
                if (IsFailed(result))
                {
                    LogWarning(
                        "PiUpdateService: MirrorChyan request failed: " + base);
                    continue;
                }
                if (status != 200)
                {
                    LogWarning(
                        "PiUpdateService: MirrorChyan returned status "
                        + std::to_string(status));
                    continue;
                }
                auto data = ParseMirrorChyanResponse(body);
                if (!data)
                {
                    continue;
                }
                if (data->code == 0)
                {
                    if (data->version_name.empty())
                    {
                        continue;
                    }
                    out.version_name = data->version_name;
                    out.release_note = data->release_note;
                    out.has_update =
                        CompareVersions(data->version_name, meta.version) > 0;
                    // D-89-03 Open Question 5：只消费 full 更新包；incremental
                    // 不采用镜像下载链接，由调用方降级走 GitHub 全量。
                    if (data->update_type != "incremental"
                        && !data->url.empty())
                    {
                        out.download_url = data->url;
                        out.sha256 =
                            data->sha256.empty()
                                ? std::optional<std::string>{}
                                : std::optional<std::string>(data->sha256);
                    }
                    return DAS_S_OK;
                }
                // code != 0 但 data.version_name 存在 → 按 MXU 语义返回部分
                // PiUpdateInfo + error_code（无下载链接）。
                if (data->has_data && !data->version_name.empty())
                {
                    out.version_name = data->version_name;
                    out.release_note = data->release_note;
                    out.has_update =
                        CompareVersions(data->version_name, meta.version) > 0;
                    out.error_code = data->code;
                    return DAS_S_OK;
                }
                // code != 0 且无版本信息 → 尝试备用 base。
                LogWarning(
                    "PiUpdateService: MirrorChyan code="
                    + std::to_string(data->code) + " msg=" + data->msg);
            }
            return DAS_E_MAAPI_UPDATE_CHECK_FAILED;
        }

        DasResult CheckGithub(
            const InterfaceMetadata& meta,
            PiHttpClient&            http,
            const std::string&       proxy,
            PiUpdateInfo&            out)
        {
            std::string owner;
            std::string repo;
            if (!ParseGithubOwnerRepo(meta.github, owner, repo))
            {
                LogError(
                    "PiUpdateService: cannot parse github url: " + meta.github);
                return DAS_E_MAAPI_UPDATE_CHECK_FAILED;
            }

            const std::string api_base = std::string(kGithubApiBase);
            const std::string web_base = std::string(kGithubWebBase);

            // Open Question 3 决策：先 releases/latest；version_name 与 latest
            // tag 不匹配时再 releases/tags/{version} 兜底。
            auto fetch_release = [&](const std::string& tag_suffix)
                -> std::optional<GithubRelease>
            {
                const std::string url =
                    api_base + "/repos/" + owner + "/" + repo + tag_suffix;
                std::string body;
                int         status = 0;
                const auto  result = http.Get(url, body, status, proxy);
                if (IsFailed(result) || status != 200)
                {
                    return std::nullopt;
                }
                return ParseGithubRelease(body);
            };

            auto release = fetch_release("/releases/latest");
            if (release && !release->tag_name.empty())
            {
                const std::string tag = release->tag_name;
                const std::string target =
                    !out.version_name.empty() ? out.version_name : tag;
                if (CompareVersions(tag, target) != 0)
                {
                    auto tagged =
                        fetch_release("/releases/tags/" + UrlEncode(target));
                    if (tagged && !tagged->tag_name.empty())
                    {
                        release = std::move(tagged);
                    }
                }
            }

            if (release && !release->tag_name.empty())
            {
                const std::string tag = release->tag_name;
                out.version_name = tag;
                out.release_note = release->body;
                out.has_update = CompareVersions(tag, meta.version) > 0;
                const std::string asset_url = MatchGithubAsset(
                    release->assets,
                    Lower(meta.name),
                    "windows",
                    "x86_64");
                if (!asset_url.empty())
                {
                    out.download_url = asset_url;
                    return DAS_S_OK;
                }
                LogWarning(
                    "PiUpdateService: no matching GitHub asset for "
                    "os=windows arch=x86_64");
            }

            // API 失败或无匹配 asset → buildDirectDownloadUrl + HEAD 探测。
            const std::string direct_url = BuildDirectDownloadUrl(
                owner,
                repo,
                meta,
                !out.version_name.empty() ? out.version_name : meta.version,
                "windows",
                "x86_64",
                web_base);
            int        head_status = 0;
            const auto head_result = http.Head(direct_url, head_status, proxy);
            if (IsOk(head_result) && head_status == 200)
            {
                out.download_url = direct_url;
                out.has_update =
                    CompareVersions(
                        !out.version_name.empty() ? out.version_name
                                                  : meta.version,
                        meta.version)
                    > 0;
                return DAS_S_OK;
            }
            LogWarning(
                "PiUpdateService: GitHub direct download URL unavailable: "
                + direct_url);
            return DAS_E_MAAPI_UPDATE_CHECK_FAILED;
        }
    } // namespace

    DasResult PiUpdateService::CheckUpdate(
        const std::filesystem::path& pi_dir,
        PiHttpClient&                http,
        PiUpdateInfo&                out,
        std::string_view             proxy_url)
    {
        out = PiUpdateInfo{};
        const std::string proxy =
            proxy_url.empty() ? ReadManifestProxyUrl() : std::string(proxy_url);

        InterfaceMetadata meta;
        const auto        metadata_result = ReadInterfaceMetadata(pi_dir, meta);
        if (IsFailed(metadata_result))
        {
            return metadata_result;
        }

        // MirrorChyan 优先（D-89-02）。
        if (!meta.mirrorchyan_rid.empty())
        {
            const auto mirror_result = CheckMirrorChyan(meta, http, proxy, out);
            if (IsOk(mirror_result))
            {
                // code!=0 部分信息 → 按 MXU 语义直接返回（不尝试 GitHub）。
                if (out.error_code)
                {
                    return DAS_S_OK;
                }
                // 无更新 → 直接返回。
                if (!out.has_update)
                {
                    return DAS_S_OK;
                }
                // 有更新且拿到全量下载链接 → 直接返回。
                if (!out.download_url.empty())
                {
                    return DAS_S_OK;
                }
                // has_update 但无下载链接（incremental 降级 / 无 url）→
                // 降级走 GitHub 全量兜底。
            }
            // MirrorChyan 失败（网络 / 无版本信息）→ GitHub 兜底。
        }

        // GitHub 兜底（D-89-02 / [ASSUMED A2]）。
        if (!meta.github.empty())
        {
            const auto github_result = CheckGithub(meta, http, proxy, out);
            if (IsOk(github_result))
            {
                return DAS_S_OK;
            }
            // GitHub 兜底失败 → 落到底部统一错误返回。
        }

        // 无可用来源 / 增量更新暂不支持且无 GitHub 兜底。
        if (!meta.github.empty())
        {
            LogWarning("PiUpdateService: 增量更新暂不支持且 GitHub 兜底失败");
        }
        else
        {
            LogError(
                "PiUpdateService: no update source (github / "
                "mirrorchyan_rid) and no usable download info");
        }
        return DAS_E_MAAPI_UPDATE_CHECK_FAILED;
    }
} // namespace Das::Plugins::DasMaaPi
