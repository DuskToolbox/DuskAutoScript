#include "PiUpdateService.h"
#include "PiSemver.h"

#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zlib.h>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        // MirrorChyan 检查 / GitHub 兜底 / 直接下载拼接的端到端默认值
        // （MXU updateService.ts:74-77 MIRRORCHYAN_API_BASES 对齐）。
        constexpr std::string_view MIRRORCHYAN_BASE_PRIMARY =
            "https://mirrorchyan.com/api/resources";
        constexpr std::string_view MIRRORCHYAN_BASE_BACKUP =
            "https://mirrorchyan.net/api/resources";
        constexpr std::string_view GITHUB_API_BASE = "https://api.github.com";
        constexpr std::string_view GITHUB_WEB_BASE = "https://github.com";

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
            constexpr std::string_view HEX_DIGITS = "0123456789ABCDEF";
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
                    out.push_back(HEX_DIGITS[c >> 4]);
                    out.push_back(HEX_DIGITS[c & 0xF]);
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

        void LogInfo(const std::string& message)
        {
            DAS_LOG_INFO(message.c_str());
        }

        // 下载 URL 记日志时剥掉 query（cdk 等敏感参数不进日志）。
        std::string RedactDownloadUrlForLog(const std::string& url)
        {
            const auto query = url.find('?');
            return query == std::string::npos ? url : url.substr(0, query);
        }

        // 把已落盘文件读回内存（流式下载完成后再读回供解压，避免下载期间
        // 整体缓冲）。
        bool ReadFileToBuffer(
            const std::filesystem::path& path,
            std::string&                 out)
        {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs)
            {
                return false;
            }
            out.assign(
                std::istreambuf_iterator<char>(ifs),
                std::istreambuf_iterator<char>());
            return true;
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
            const std::string&       cdk_key,
            PiUpdateInfo&            out)
        {
            const std::vector<std::string> bases{
                std::string(MIRRORCHYAN_BASE_PRIMARY),
                std::string(MIRRORCHYAN_BASE_BACKUP)};
            for (const auto& base : bases)
            {
                std::string url =
                    base + "/" + meta.mirrorchyan_rid
                    + "/latest?current_version=" + UrlEncode(meta.version)
                    + "&user_agent=DasMaaPi&os=windows"
                      "&arch=x86_64&channel=stable";
                // 对齐 MXU 语义：CDK 作为可选 query 参数（cdk）；无 key
                // 时不带。
                if (!cdk_key.empty())
                {
                    url += "&cdk=" + UrlEncode(cdk_key);
                }
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

            const std::string api_base = std::string(GITHUB_API_BASE);
            const std::string web_base = std::string(GITHUB_WEB_BASE);

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

        // -----------------------------------------------------------------
        // ZIP 解压（T-89-02-01 目录穿越防护）。格式解析参考
        // PluginZipExtractor.cpp:89-152 / 160-354 的 IsPathSafe /
        // EnumerateZipEntries / ExtractEntryToDisk 模式。
        // -----------------------------------------------------------------

        constexpr std::uint32_t LOCAL_FILE_HEADER_SIG = 0x04034B50;
        constexpr std::uint32_t CENTRAL_DIR_SIG = 0x02014B50;
        constexpr std::uint32_t END_OF_CENTRAL_DIR_SIG = 0x06054B50;

        // 单个 zip 条目解压后的大小上限（解压炸弹防护）。HTTP 响应体已不设
        // 上限，但攻击者/上游可控的 uncompressed_size（uint32，最高 ~4GB）
        // 不能直接触发 output.resize() 的巨量分配，故单条目仍限 512MB。
        constexpr std::size_t MAX_UNCOMPRESSED_ZIP_ENTRY_BYTES =
            512u * 1024u * 1024u;

        template <typename T>
        T ReadLE(const char* data, size_t offset)
        {
            T value;
            std::memcpy(&value, data + offset, sizeof(T));
            return value;
        }

        bool FindEndOfCentralDir(
            const std::string& zip_data,
            size_t&            eocd_offset)
        {
            constexpr size_t EOCD_SIZE = 22;
            constexpr size_t MAX_COMMENT_SIZE = 65535;
            if (zip_data.size() < EOCD_SIZE)
            {
                return false;
            }
            const size_t max_search =
                std::min(zip_data.size(), EOCD_SIZE + MAX_COMMENT_SIZE);
            // max_search >= EOCD_SIZE guaranteed above, so the loop count does
            // not underflow (unsigned arithmetic) on short inputs.
            for (size_t i = 0; i <= max_search - EOCD_SIZE; ++i)
            {
                const size_t offset = zip_data.size() - EOCD_SIZE - i;
                if (ReadLE<std::uint32_t>(zip_data.data(), offset)
                    == END_OF_CENTRAL_DIR_SIG)
                {
                    eocd_offset = offset;
                    return true;
                }
            }
            return false;
        }

        // 目录穿越防护：条目路径规范化后必须位于 extract_dir 内。
        // 拒绝绝对路径 / 含 ".." 段 / 规范化后逃逸出 extract_dir（双保险，
        // 参考 Phase 88 SanitizeImagePath 范式 + PluginZipExtractor
        // IsPathSafe）。
        bool IsSafeZipEntry(
            const std::filesystem::path& extract_dir,
            std::string_view             entry_name)
        {
            if (entry_name.empty())
            {
                return false;
            }
            if (entry_name.front() == '/' || entry_name.front() == '\\')
            {
                return false;
            }
            // 逐分量检查 ".."（同时处理 '/' 和 '\\'）。
            for (const char* c = entry_name.data();
                 c != entry_name.data() + entry_name.size();)
            {
                const char* next =
                    std::find(c, entry_name.data() + entry_name.size(), '/');
                std::string_view component(c, next - c);
                if (component == "..")
                {
                    return false;
                }
                if (next == entry_name.data() + entry_name.size())
                {
                    break;
                }
                c = next + 1;
            }
            // 规范化后必须在 extract_dir 内（双保险）。
            const auto target = (extract_dir / entry_name).lexically_normal();
            const auto normalized_extract = extract_dir.lexically_normal();
            const auto rel = target.lexically_relative(normalized_extract);
            if (rel.empty())
            {
                return false;
            }
            const std::string rel_str = rel.generic_string();
            if (rel_str == ".." || rel_str.rfind("../", 0) == 0)
            {
                return false;
            }
            if (target.string().rfind(normalized_extract.string(), 0) != 0
                && target != normalized_extract)
            {
                return false;
            }
            return true;
        }

        bool InflateZipEntry(
            const char*  compressed,
            size_t       compressed_size,
            size_t       uncompressed_size,
            std::string& output)
        {
            // WR-02: reject entries that claim to inflate beyond the project
            // cap before allocating, so a hostile/broken package cannot OOM.
            if (uncompressed_size > MAX_UNCOMPRESSED_ZIP_ENTRY_BYTES)
            {
                return false;
            }
            output.resize(uncompressed_size);
            z_stream stream{};
            stream.next_in =
                reinterpret_cast<Bytef*>(const_cast<char*>(compressed));
            stream.avail_in = static_cast<uInt>(compressed_size);
            stream.next_out = reinterpret_cast<Bytef*>(output.data());
            stream.avail_out = static_cast<uInt>(uncompressed_size);
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
            {
                return false;
            }
            const int ret = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            return ret == Z_STREAM_END;
        }

        // 解压 zip 到 extract_dir，返回 DAS_S_OK /
        // DAS_E_MAAPI_UPDATE_APPLY_FAILED。
        DasResult ExtractZip(
            const std::string&           zip_data,
            const std::filesystem::path& extract_dir)
        {
            size_t eocd_offset = 0;
            if (!FindEndOfCentralDir(zip_data, eocd_offset))
            {
                LogError("PiUpdateService: invalid zip (no EOCD)");
                return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
            }
            const auto num_entries =
                ReadLE<std::uint16_t>(zip_data.data(), eocd_offset + 10);
            const auto cd_offset =
                ReadLE<std::uint32_t>(zip_data.data(), eocd_offset + 16);

            std::error_code ec;
            // 空 zip（0 条目）不创建 extract_dir —— replace rename 的源目录
            // 缺失 → ApplyUpdate 视为替换失败走回滚，旧目录被恢复。
            if (num_entries == 0)
            {
                LogError("PiUpdateService: empty zip has no files to extract");
                return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
            }
            std::filesystem::create_directories(extract_dir, ec);

            size_t pos = cd_offset;
            for (std::uint16_t i = 0; i < num_entries; ++i)
            {
                if (pos + 46 > zip_data.size())
                {
                    LogError("PiUpdateService: truncated central directory");
                    return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                }
                if (ReadLE<std::uint32_t>(zip_data.data(), pos)
                    != CENTRAL_DIR_SIG)
                {
                    LogError(
                        "PiUpdateService: bad central directory signature");
                    return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                }
                const auto compression =
                    ReadLE<std::uint16_t>(zip_data.data(), pos + 10);
                const auto compressed_size =
                    ReadLE<std::uint32_t>(zip_data.data(), pos + 20);
                const auto uncompressed_size =
                    ReadLE<std::uint32_t>(zip_data.data(), pos + 24);
                const auto filename_length =
                    ReadLE<std::uint16_t>(zip_data.data(), pos + 28);
                const auto extra_length =
                    ReadLE<std::uint16_t>(zip_data.data(), pos + 30);
                const auto comment_length =
                    ReadLE<std::uint16_t>(zip_data.data(), pos + 32);
                const auto local_header_offset =
                    ReadLE<std::uint32_t>(zip_data.data(), pos + 42);

                if (pos + 46 + filename_length + extra_length + comment_length
                    > zip_data.size())
                {
                    LogError("PiUpdateService: truncated entry metadata");
                    return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                }

                const std::string filename(
                    zip_data.data() + pos + 46,
                    filename_length);
                pos += 46 + filename_length + extra_length + comment_length;

                // 目录穿越防护（T-89-02-01）。
                if (!IsSafeZipEntry(extract_dir, filename))
                {
                    LogError(
                        "PiUpdateService: unsafe zip entry rejected: "
                        + filename);
                    return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                }

                const bool is_directory =
                    !filename.empty() && filename.back() == '/';
                const auto target = (extract_dir / filename).lexically_normal();
                if (is_directory)
                {
                    std::filesystem::create_directories(target, ec);
                    continue;
                }
                std::filesystem::create_directories(target.parent_path(), ec);

                // 从 local file header 解析数据偏移。
                if (local_header_offset + 30 > zip_data.size())
                {
                    LogError("PiUpdateService: truncated local header");
                    return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                }
                if (ReadLE<std::uint32_t>(zip_data.data(), local_header_offset)
                    != LOCAL_FILE_HEADER_SIG)
                {
                    LogError("PiUpdateService: bad local header signature");
                    return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                }
                const auto local_filename_length = ReadLE<std::uint16_t>(
                    zip_data.data(),
                    local_header_offset + 26);
                const auto local_extra_length = ReadLE<std::uint16_t>(
                    zip_data.data(),
                    local_header_offset + 28);
                const size_t data_offset = local_header_offset + 30
                                           + local_filename_length
                                           + local_extra_length;
                if (data_offset + compressed_size > zip_data.size())
                {
                    LogError("PiUpdateService: truncated file data");
                    return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                }

                std::ofstream ofs(target, std::ios::binary | std::ios::trunc);
                if (!ofs)
                {
                    LogError("PiUpdateService: cannot open output file");
                    return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                }
                if (compression == 0)
                {
                    ofs.write(zip_data.data() + data_offset, compressed_size);
                }
                else if (compression == 8)
                {
                    // WR-02: 解压炸弹防护——解压目标大小超上限直接拒绝
                    // （uncompressed_size 来自 zip central directory，攻击者/
                    // 上游可控，最高 ~4GB）。
                    if (uncompressed_size > MAX_UNCOMPRESSED_ZIP_ENTRY_BYTES)
                    {
                        LogError(
                            "PiUpdateService: zip entry uncompressed size "
                            "exceeds limit: "
                            + std::to_string(uncompressed_size));
                        return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                    }
                    std::string output;
                    if (!InflateZipEntry(
                            zip_data.data() + data_offset,
                            compressed_size,
                            uncompressed_size,
                            output))
                    {
                        LogError("PiUpdateService: inflate failed");
                        return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                    }
                    ofs.write(output.data(), output.size());
                }
                else
                {
                    LogError(
                        "PiUpdateService: unsupported zip compression: "
                        + std::to_string(compression));
                    return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                }
            }
            return DAS_S_OK;
        }

        // 当前项目目录内 interface.json 的版本号（用于备份目录名）。
        std::string ReadCurrentVersion(const std::filesystem::path& pi_dir)
        {
            InterfaceMetadata meta;
            if (IsFailed(ReadInterfaceMetadata(pi_dir, meta)))
            {
                return {};
            }
            return meta.version;
        }

        // 逐文件替换（被锁文件暂存 .pending/ 策略）。extract_dir 内
        // 每个文件覆盖 pi_dir 对应文件；无法覆盖（Windows sharing violation /
        // std::filesystem_error）时把新版本移入 {pi_dir}/.pending/{rel}
        // 并警告， 其余文件正常替换。interface.json 自身被锁（session 正持有）→
        // 整体 失败返回 -10008，pi_dir 保持完整（含原 interface.json）。
        //
        // WR-03 加固：1) 先固定完整文件清单并确保 interface.json 最先处理——
        // 若它被锁，在任何其他文件被触碰前即返回 -10008，旧目录保持完整，满足
        // 头文件"-10008 时旧目录已回滚"契约；2) 目标存在时先 rename 到侧备份
        // 再 rename 新文件，remove 成功但 rename 失败的分支不再导致原文件从规范
        // 路径消失（可恢复侧备份，避免文件丢失）。
        DasResult ReplaceFilesWithPending(
            const std::filesystem::path& extract_dir,
            const std::filesystem::path& pi_dir)
        {
            std::vector<std::filesystem::path> files;
            std::error_code                    ec;
            for (std::filesystem::recursive_directory_iterator it(extract_dir),
                 end;
                 it != end;
                 ++it)
            {
                if (it->is_directory())
                {
                    continue;
                }
                files.push_back(it->path());
            }
            // interface.json 优先处理（stable_partition 保序，覆盖顺序确定）。
            std::stable_partition(
                files.begin(),
                files.end(),
                [](const std::filesystem::path& p)
                { return p.filename() == "interface.json"; });

            std::vector<std::string> pending_files;
            for (const auto& src : files)
            {
                const auto rel = src.lexically_relative(extract_dir);
                const auto target = pi_dir / rel;
                std::filesystem::create_directories(target.parent_path(), ec);

                // 目标存在 → 先 rename 到侧备份（不直接删除），这样新文件
                // rename 失败时仍能恢复原文件（WR-03 防文件丢失）。
                const auto      backup = target.string() + ".update-backup";
                std::error_code backup_ec;
                if (std::filesystem::exists(target))
                {
                    std::filesystem::rename(target, backup, backup_ec);
                }
                std::error_code rename_ec;
                std::filesystem::rename(src, target, rename_ec);
                if (!rename_ec)
                {
                    // 新文件就位，删除侧备份（备份移动失败则忽略，原文件仍在
                    // 规范路径）。
                    if (!backup_ec && std::filesystem::exists(backup))
                    {
                        std::filesystem::remove(backup, ec);
                    }
                    continue;
                }
                if (rel == std::filesystem::path("interface.json"))
                {
                    // interface.json 最先处理：失败时无其他文件被替换，pi_dir
                    // 保持完整（恢复侧备份，若备份移动失败则原文件未动）。
                    if (!backup_ec && std::filesystem::exists(backup))
                    {
                        std::filesystem::rename(backup, target, ec);
                    }
                    LogError(
                        "PiUpdateService: interface.json is locked, update "
                        "failed");
                    return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
                }
                // rename 新文件失败 → 恢复侧备份，再把新版本暂存 .pending/。
                if (!backup_ec && std::filesystem::exists(backup))
                {
                    std::filesystem::rename(backup, target, ec);
                }
                // 被锁（sharing violation）→ 暂存 .pending/ + 警告。
                const auto pending_path = pi_dir / ".pending" / rel;
                std::filesystem::create_directories(
                    pending_path.parent_path(),
                    ec);
                std::error_code copy_ec;
                std::filesystem::copy_file(
                    src,
                    pending_path,
                    std::filesystem::copy_options::overwrite_existing,
                    copy_ec);
                pending_files.push_back(rel.string());
            }
            if (!pending_files.empty())
            {
                std::string joined;
                for (const auto& f : pending_files)
                {
                    if (!joined.empty())
                    {
                        joined += ", ";
                    }
                    joined += f;
                }
                LogWarning(
                    "PiUpdateService: files staged to .pending (locked): "
                    + joined);
            }
            return DAS_S_OK;
        }

        // resource.hash 校验（D-89-05 / [ASSUMED A5]）：替换成功后用新
        // interface.json 的 resource 路径创建并加载资源，取 MaaFW hash 与
        // 更新包记录 sha256 比对。不匹配或缺失 → 警告「建议重新下载资源包」，
        // 不硬失败（协议 §resource.hash 语义）；无法计算（无 resource 句柄 /
        // 加载失败）→ 降级为警告。
        void VerifyResourceHash(
            const std::filesystem::path& pi_dir,
            const PiUpdateInfo&          info,
            IMaaApiBoundary&             boundary)
        {
            const auto parsed = ParseProjectInterface(
                {.interface_path = pi_dir / "interface.json"});
            if (!parsed.ok)
            {
                LogWarning(
                    "PiUpdateService: cannot parse new interface.json for "
                    "resource.hash verify");
                return;
            }
            std::vector<std::filesystem::path> resource_paths;
            for (const auto& resource : parsed.catalog.resources)
            {
                resource_paths.insert(
                    resource_paths.end(),
                    resource.resolved_paths.begin(),
                    resource.resolved_paths.end());
            }
            if (resource_paths.empty())
            {
                return; // 无 resource 可校验
            }

            const auto resource = boundary.CreateResource();
            if (resource == kInvalidMaaResourceHandle)
            {
                LogWarning(
                    "PiUpdateService: cannot create resource for hash "
                    "verification");
                return;
            }
            bool loaded = true;
            for (const auto& path : resource_paths)
            {
                const auto load =
                    boundary.LoadResource(resource, path.string());
                if (!load.ok)
                {
                    loaded = false;
                    break;
                }
            }
            if (!loaded)
            {
                boundary.DestroyResource(resource);
                LogWarning(
                    "PiUpdateService: resource load failed, hash check "
                    "skipped");
                return;
            }
            const auto actual = boundary.GetResourceHash(resource);
            boundary.DestroyResource(resource);
            if (!actual)
            {
                LogWarning("PiUpdateService: cannot compute resource hash");
                return;
            }
            if (!info.sha256 || *actual != *info.sha256)
            {
                LogWarning(
                    "PiUpdateService: resource hash mismatch — 建议重新下载"
                    "资源包");
            }
        }
    } // namespace

    DasResult PiUpdateService::CheckUpdate(
        const std::filesystem::path& pi_dir,
        PiHttpClient&                http,
        PiUpdateInfo&                out,
        std::string_view             proxy_url,
        std::string_view             mirrorchyan_cdk,
        std::string_view             update_channel)
    {
        out = PiUpdateInfo{};
        const std::string proxy(proxy_url);
        const std::string cdk_key(mirrorchyan_cdk);

        InterfaceMetadata meta;
        const auto        metadata_result = ReadInterfaceMetadata(pi_dir, meta);
        if (IsFailed(metadata_result))
        {
            return metadata_result;
        }

        // 检查更新通道（adapter.updateChannel）：
        // "github" → 仅 GitHub 单通道检查（不触碰 MirrorChyan）；
        // 空 / "mirrorchyan" → MirrorChyan 优先 + GitHub 兜底（既有行为）。
        if (update_channel == "github")
        {
            if (meta.github.empty())
            {
                LogError(
                    "PiUpdateService: updateChannel=github but interface.json "
                    "has no github source");
                return DAS_E_MAAPI_UPDATE_CHECK_FAILED;
            }
            const auto github_result = CheckGithub(meta, http, proxy, out);
            if (IsOk(github_result))
            {
                return DAS_S_OK;
            }
            return DAS_E_MAAPI_UPDATE_CHECK_FAILED;
        }

        // MirrorChyan 优先（D-89-02）。
        if (!meta.mirrorchyan_rid.empty())
        {
            const auto mirror_result =
                CheckMirrorChyan(meta, http, proxy, cdk_key, out);
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

    DasResult PiUpdateService::ApplyUpdate(
        const std::filesystem::path& pi_dir,
        const PiUpdateInfo&          info,
        PiHttpClient&                http,
        std::string_view             proxy_url,
        std::string_view             mirrorchyan_cdk,
        IMaaApiBoundary*             boundary,
        ProgressReporter             progress_reporter)
    {
        IMaaApiBoundary& api_boundary =
            boundary ? *boundary : DefaultMaaApiBoundary();
        if (info.download_url.empty())
        {
            LogError("PiUpdateService: ApplyUpdate requires a download_url");
            return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
        }
        const std::string proxy(proxy_url);
        const std::string old_version = ReadCurrentVersion(pi_dir);
        const std::string entry_id =
            info.version_name.empty() ? old_version : info.version_name;
        const std::string log_url = RedactDownloadUrlForLog(info.download_url);

        // .update 临时目录放 pi_dir 的兄弟目录（不是 pi_dir 内）——否则备份
        // rename(pi_dir → backup) 会把 .update 一起移走，后续 replace rename
        // 的源路径失效。
        const auto update_root =
            pi_dir.parent_path() / (pi_dir.filename().string() + ".update");
        const auto version_tag =
            info.version_name.empty() ? old_version : info.version_name;
        const auto zip_path = update_root / (version_tag + ".zip");
        const auto extract_dir = update_root / ("extract_" + version_tag);
        const auto backup_dir =
            std::filesystem::path(pi_dir.string() + "_backup_" + old_version);

        // RAII：退出时清理 .update 临时目录（zip + extract）。
        struct UpdateCleanup
        {
            std::filesystem::path root;
            bool                  dismissed = false;
            ~UpdateCleanup()
            {
                if (!dismissed)
                {
                    std::error_code ec;
                    std::filesystem::remove_all(root, ec);
                }
            }
        } cleanup{update_root};

        // 1. 流式下载 zip 到 .update/{version}.zip（边下边写盘，不整体缓冲）。
        // MirrorChyan 来源的下载 URL 携带 CDK 时附带 cdk query（对齐 MXU
        // 语义）；GitHub 来源或 CDK 为空时不改动 URL。
        const std::string cdk_key(mirrorchyan_cdk);
        std::string       download_url(info.download_url);
        if (!cdk_key.empty()
            && download_url.find("mirrorchyan") != std::string::npos)
        {
            download_url +=
                std::string(
                    download_url.find('?') != std::string::npos ? "&" : "?")
                + "cdk=" + UrlEncode(cdk_key);
        }
        std::error_code ec;
        std::filesystem::create_directories(update_root, ec);

        // 可追溯日志（entryId / 下载 URL / 阶段 / 字节 / 错误码 / 耗时）。
        LogInfo(
            DAS::fmt::format(
                "PiUpdateService: update download start entryId={} url={}",
                entry_id,
                log_url));
        // 速率采样 + 上报节流（~4 Hz）：首块必报，之后每 ≥250ms 报一次，
        // 最后一块（bytes==total）必报。
        DownloadRateTracker rate_tracker;
        const auto          download_start = rate_tracker.Now();
        auto                last_report = download_start;
        bool                first_download_report = true;
        int                 download_status = 0;
        const auto          download_result = http.DownloadStream(
            download_url,
            zip_path,
            [&](const DownloadProgress& p)
            {
                rate_tracker.OnChunk(p.bytes_received);
                const auto now = rate_tracker.Now();
                const bool should_report =
                    first_download_report
                    || (now - last_report) >= std::chrono::milliseconds(250)
                    || (p.total_bytes && p.bytes_received >= *p.total_bytes);
                if (should_report && progress_reporter)
                {
                    UpdateProgress up{};
                    up.stage = UpdateStage::Download;
                    up.bytes_received = p.bytes_received;
                    up.total_bytes = p.total_bytes;
                    up.rate_mbps = rate_tracker.CurrentRateMbps();
                    up.elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - download_start);
                    progress_reporter(up);
                    last_report = now;
                }
                first_download_report = false;
            },
            download_status,
            proxy);
        const auto download_elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                rate_tracker.Now() - download_start)
                .count();
        if (IsFailed(download_result) || download_status != 200)
        {
            LogError(
                DAS::fmt::format(
                    "PiUpdateService: update download failed entryId={} url={} "
                    "status={} result={} elapsedMs={}",
                    entry_id,
                    log_url,
                    download_status,
                    static_cast<int>(download_result),
                    download_elapsed_ms));
            return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
        }
        LogInfo(
            DAS::fmt::format(
                "PiUpdateService: update download complete entryId={} zip={} "
                "elapsedMs={}",
                entry_id,
                zip_path.filename().string(),
                download_elapsed_ms));

        // 流式下载完成后再读回 zip 文件供解压（此阶段一次性缓冲）。
        std::string zip_data;
        if (!ReadFileToBuffer(zip_path, zip_data))
        {
            LogError(
                DAS::fmt::format(
                    "PiUpdateService: cannot read downloaded zip entryId={}",
                    entry_id));
            return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
        }

        // 2. 安全解压（目录穿越防护）。
        if (progress_reporter)
        {
            progress_reporter(
                UpdateProgress{UpdateStage::Extract, 0, std::nullopt});
        }
        const auto extract_result = ExtractZip(zip_data, extract_dir);
        if (IsFailed(extract_result))
        {
            LogError(
                DAS::fmt::format(
                    "PiUpdateService: update extract failed entryId={} result={}",
                    entry_id,
                    static_cast<int>(extract_result)));
            return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
        }

        // 3. 备份 + 原子替换（rename 替换 + 回滚模式）。若
        //    {pi_dir}_backup_{oldVersion} 已存在先删除（仅保留上一份）。
        if (progress_reporter)
        {
            progress_reporter(
                UpdateProgress{UpdateStage::Replace, 0, std::nullopt});
        }
        LogInfo(
            DAS::fmt::format(
                "PiUpdateService: update backup start entryId={} backup={}",
                entry_id,
                backup_dir.filename().string()));
        std::filesystem::remove_all(backup_dir, ec);
        std::filesystem::rename(pi_dir, backup_dir, ec);
        if (ec)
        {
            // 整体 rename 失败：很可能是 pi_dir 内存在被占用的文件（如运行中
            // 的 MaaFW DLL，Windows sharing violation）。降级为逐文件替换 +
            // .pending/ 暂存新版本 + 警告；若 interface.json 自身被锁则整体
            // 失败（pi_dir 未被改动，保持完整）。
            LogWarning(
                DAS::fmt::format(
                    "PiUpdateService: update backup failed entryId={} falling "
                    "back to per-file replace: {}",
                    entry_id,
                    ec.message()));
            // 回滚：rename 失败时 pi_dir 保持原样（未移动），无需恢复。
            const auto fallback_result =
                ReplaceFilesWithPending(extract_dir, pi_dir);
            LogInfo(
                DAS::fmt::format(
                    "PiUpdateService: update replace (per-file) finished "
                    "entryId={} result={}",
                    entry_id,
                    static_cast<int>(fallback_result)));
            cleanup.dismissed = true;
            std::filesystem::remove_all(update_root, ec);
            return fallback_result;
        }
        LogInfo(
            DAS::fmt::format(
                "PiUpdateService: update backup complete entryId={} backup={}",
                entry_id,
                backup_dir.filename().string()));
        LogInfo(
            DAS::fmt::format(
                "PiUpdateService: update replace start entryId={}",
                entry_id));
        std::filesystem::rename(extract_dir, pi_dir, ec);
        if (ec)
        {
            // 任一步失败 → 回滚：恢复旧目录，清理 .update 临时目录。
            LogError(
                DAS::fmt::format(
                    "PiUpdateService: update replace failed entryId={} rolling "
                    "back: {}",
                    entry_id,
                    pi_dir.string()));
            std::error_code restore_ec;
            std::filesystem::rename(backup_dir, pi_dir, restore_ec);
            return DAS_E_MAAPI_UPDATE_APPLY_FAILED;
        }
        LogInfo(
            DAS::fmt::format(
                "PiUpdateService: update replace complete entryId={}",
                entry_id));

        // 4. 成功：resource.hash 校验（警告不硬失败）+ 清理 .update
        //    临时目录（zip + extract），保留 backup。
        VerifyResourceHash(pi_dir, info, api_boundary);
        cleanup.dismissed = true;
        std::filesystem::remove_all(update_root, ec);
        LogInfo(
            DAS::fmt::format(
                "PiUpdateService: update applied entryId={} elapsedMs={}",
                entry_id,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    rate_tracker.Now() - download_start)
                    .count()));
        return DAS_S_OK;
    }
} // namespace Das::Plugins::DasMaaPi
