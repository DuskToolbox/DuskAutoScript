#include <das/Core/ForeignInterfaceHost/PluginZipExtractor.h>

// cpp_yyjson.hpp 内部使用 assert 但未自带 <cassert>，需要本翻译单元
// 先行提供（本文件自身无 assert 调用，勿当死包含清理）。
#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <das/Utils/StringUtils.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <zip.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    bool IsPathSafe(
        const std::filesystem::path& target_dir,
        const std::filesystem::path& entry_path)
    {
        std::error_code ec;
        auto            canonical_target =
            std::filesystem::weakly_canonical(target_dir, ec);
        if (ec)
        {
            return false;
        }
        auto canonical_entry =
            std::filesystem::weakly_canonical(entry_path, ec);
        if (ec)
        {
            return false;
        }

        auto relative = canonical_entry.lexically_relative(canonical_target);
        auto rel_str = std::string{DAS::Utils::U8AsString(relative.u8string())};
        if (rel_str.size() >= 2 && rel_str.substr(0, 2) == "..")
        {
            return false;
        }
        if (rel_str == ".")
        {
            return false;
        }
        return !rel_str.empty();
    }

    // Windows 保留设备名（大小写不敏感精确匹配）：CON / PRN / AUX /
    // NUL / COM1-9 / LPT1-9。
    bool IsReservedDeviceName(const std::string& component)
    {
        static constexpr std::array<std::string_view, 22> kReserved = {
            "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
            "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
            "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
        std::string upper;
        upper.reserve(component.size());
        for (const char c : component)
        {
            upper.push_back(
                static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
        return std::find(kReserved.begin(), kReserved.end(), upper)
               != kReserved.end();
    }

    // 组件级安全检查：拒绝 ".."、空组件（连续分隔符）、含 ':'（NTFS
    // 备用数据流 / 驱动器相对名）、尾点 / 尾空格（Win32 落盘剥离产生
    // 别名）与保留设备名。
    bool IsPathComponentSafe(const std::string& component)
    {
        if (component.empty() || component == "..")
        {
            return false;
        }
        if (component.find(':') != std::string::npos)
        {
            return false;
        }
        if (component.back() == '.' || component.back() == ' ')
        {
            return false;
        }
        return !IsReservedDeviceName(component);
    }

    // 目录名单组件安全（flat 包自动包装目录名派生用）：非空、不含任一
    // 分隔符且通过组件级检查。
    bool IsSinglePathComponentSafe(const std::string& name)
    {
        return name.find('/') == std::string::npos
               && name.find('\\') == std::string::npos
               && IsPathComponentSafe(name);
    }

    // 整档首道文件名守卫：'\' 与 '/' 交错夹带 '..' / 设备名 / ADS 名等
    // 组件在此拒绝（磁盘零写入）。归一化仅用于检查——落盘路径构造仍用
    // 原始条目名，目录穿越 containment 由 IsPathSafe 二道兜底。
    bool IsFilenameSafe(const std::string& filename)
    {
        if (filename.empty())
        {
            return false;
        }

        if (filename[0] == '/' || filename[0] == '\\')
        {
            return false;
        }

        std::string normalized;
        normalized.reserve(filename.size());
        for (const char c : filename)
        {
            normalized.push_back(c == '\\' ? '/' : c);
        }

        std::istringstream ss(normalized);
        std::string        component;
        while (std::getline(ss, component, '/'))
        {
            if (!IsPathComponentSafe(component))
            {
                return false;
            }
        }

        return true;
    }

    // ---------------------------------------------------------------------
    // libzip 读取路径：容器解析 / CRC32 / zip64 / 文件名编码归一化由库
    // 承担；目录穿越防护与解压炸弹上限在本侧保留。
    // ---------------------------------------------------------------------

    // 单个 zip 条目解压后的大小上限（解压炸弹防护）。声明大小与实际
    // 大小都不得超限（声明大小来自 central directory，攻击者可控）。
    constexpr std::size_t MAX_UNCOMPRESSED_ZIP_ENTRY_BYTES =
        512u * 1024u * 1024u;

    // 单次 zip_fread 的块大小：流式处理，不整条目驻留内存。
    constexpr zip_uint64_t ZIP_ENTRY_READ_CHUNK_BYTES = 64u * 1024u;

    // manifest 类小条目的内存读取上限：真实 manifest 是 KB 级，该上限
    // 同时杀死 metadata 侧的内存炸弹。
    constexpr std::size_t MAX_MANIFEST_ENTRY_BYTES = 16u * 1024u * 1024u;

    // zip_stat_index 的单趟条目快照。
    struct EntryInfo
    {
        zip_uint64_t index;
        std::string  name;
        bool         is_directory;
        bool         size_valid;
        zip_uint64_t declared_size;
    };

    // 打开内存 zip（只读）。freep=0：libzip 不接管缓冲所有权，调用方
    // 缓冲必须存活到 archive 关闭（本文件各入口的 archive 都在 zip_data
    // 存活的函数作用域内开闭）。
    zip_t* OpenZipArchiveFromMemory(std::string_view zip_data)
    {
        zip_error_t open_error;
        zip_error_init(&open_error);
        zip_source_t* source = zip_source_buffer_create(
            zip_data.data(),
            static_cast<zip_uint64_t>(zip_data.size()),
            0,
            &open_error);
        if (source == nullptr)
        {
            DAS_CORE_LOG_WARN(
                "ZIP source create failed: {}",
                zip_error_strerror(&open_error));
            zip_error_fini(&open_error);
            return nullptr;
        }
        zip_t* archive = zip_open_from_source(source, ZIP_RDONLY, &open_error);
        if (archive == nullptr)
        {
            DAS_CORE_LOG_WARN(
                "Invalid ZIP: {}",
                zip_error_strerror(&open_error));
            zip_error_fini(&open_error);
            // 失败的 open 只 close 不 free，调用方必须补 zip_source_free。
            zip_source_free(source);
            return nullptr;
        }
        zip_error_fini(&open_error);
        return archive;
    }

    // 关闭 archive：zip_close 失败路径不释放 archive，必须补 zip_discard
    // 防泄漏（成功返回 true）。两个入口统一走本函数，消除关闭纪律分叉。
    bool CloseZipArchive(zip_t* archive)
    {
        if (zip_close(archive) != 0)
        {
            DAS_CORE_LOG_WARN("ZIP close failed: {}", zip_strerror(archive));
            zip_discard(archive);
            return false;
        }
        return true;
    }

    // 读取内核：64KB 分块读到 EOF。CRC 校验由 libzip 在条目数据读完时
    // 完成，不一致以负返回击穿——循环必须跑到 EOF，停在声明大小会漏
    // 触发校验；累计超声明值即时拒绝、EOF 后累计 != 声明值拒绝（声明
    // 大小造假双向覆盖，同时是实际大小侧的炸弹上限）。
    DasResult ReadZipEntryToSink(
        const EntryInfo&                                      entry,
        zip_file_t*                                           entry_file,
        const std::function<void(const char*, zip_uint64_t)>& sink)
    {
        std::string read_buffer(
            static_cast<std::size_t>(ZIP_ENTRY_READ_CHUNK_BYTES),
            '\0');
        zip_uint64_t total_read = 0;
        for (;;)
        {
            const zip_int64_t read_now = zip_fread(
                entry_file,
                read_buffer.data(),
                ZIP_ENTRY_READ_CHUNK_BYTES);
            if (read_now < 0)
            {
                DAS_CORE_LOG_WARN(
                    "ZIP entry read failed: {}",
                    zip_file_strerror(entry_file));
                return DAS_E_INVALID_ARGUMENT;
            }
            if (read_now == 0)
            {
                break;
            }
            total_read += static_cast<zip_uint64_t>(read_now);
            if (total_read > entry.declared_size)
            {
                DAS_CORE_LOG_WARN("ZIP entry size mismatch: {}", entry.name);
                return DAS_E_INVALID_ARGUMENT;
            }
            if (sink)
            {
                sink(read_buffer.data(), static_cast<zip_uint64_t>(read_now));
            }
        }
        if (total_read != entry.declared_size)
        {
            DAS_CORE_LOG_WARN("ZIP entry size mismatch: {}", entry.name);
            return DAS_E_INVALID_ARGUMENT;
        }
        return DAS_S_OK;
    }

    // 读取单个条目并落盘（父目录自动创建）。声明大小未知或超上限在
    // 打开条目之前拒绝。
    DasResult ReadEntryToFile(
        zip_t*                       archive,
        const EntryInfo&             entry,
        const std::filesystem::path& target_path)
    {
        if (!entry.size_valid
            || entry.declared_size > static_cast<zip_uint64_t>(
                   MAX_UNCOMPRESSED_ZIP_ENTRY_BYTES))
        {
            DAS_CORE_LOG_WARN(
                "ZIP entry uncompressed size exceeds limit or unknown: {}",
                entry.name);
            return DAS_E_INVALID_ARGUMENT;
        }

        auto parent = target_path.parent_path();
        if (!parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                // 真实病因（非法组件等）在此留痕；不提前返回，ofstream
                // 打开失败仍是 fail-closed 兜底。
                DAS_CORE_LOG_WARN(
                    "Failed to create parent directory: {} ({})",
                    DAS::Utils::U8AsString(parent.u8string()),
                    ec.message());
            }
        }

        std::ofstream ofs(target_path, std::ios::binary | std::ios::trunc);
        if (!ofs)
        {
            DAS_CORE_LOG_WARN(
                "Failed to write file: {}",
                DAS::Utils::U8AsString(target_path.u8string()));
            return DAS_E_FAIL;
        }

        zip_file_t* entry_file = zip_fopen_index(archive, entry.index, 0);
        if (entry_file == nullptr)
        {
            DAS_CORE_LOG_WARN(
                "ZIP entry open failed: {}",
                zip_strerror(archive));
            return DAS_E_INVALID_ARGUMENT;
        }

        const auto result = ReadZipEntryToSink(
            entry,
            entry_file,
            [&ofs](const char* data, zip_uint64_t size)
            { ofs.write(data, static_cast<std::streamsize>(size)); });
        zip_fclose(entry_file);
        if (result != DAS_S_OK)
        {
            return result;
        }

        // 显式 flush + close：ofstream 缓冲的尾块在析构期落盘，若不在
        // 此验证，磁盘满 / 配额场景会把截断文件当成功装进 plugin_dir。
        ofs.close();
        if (!ofs)
        {
            DAS_CORE_LOG_WARN(
                "Failed to write file data: {}",
                DAS::Utils::U8AsString(target_path.u8string()));
            return DAS_E_FAIL;
        }

        return DAS_S_OK;
    }

    // 读取单个条目到内存（manifest 类小条目用）。在读取内核的声明大小
    // 上限之外，追加调用方给定的更紧的内存上限。
    DasResult ReadEntryToString(
        zip_t*           archive,
        const EntryInfo& entry,
        std::string&     out,
        std::size_t      max_bytes)
    {
        if (!entry.size_valid
            || entry.declared_size > static_cast<zip_uint64_t>(
                   MAX_UNCOMPRESSED_ZIP_ENTRY_BYTES))
        {
            DAS_CORE_LOG_WARN(
                "ZIP entry uncompressed size exceeds limit or unknown: {}",
                entry.name);
            return DAS_E_INVALID_ARGUMENT;
        }
        if (entry.declared_size > static_cast<zip_uint64_t>(max_bytes))
        {
            DAS_CORE_LOG_WARN(
                "ZIP entry size exceeds read limit: {}",
                entry.name);
            return DAS_E_INVALID_ARGUMENT;
        }

        out.clear();
        out.reserve(static_cast<std::size_t>(entry.declared_size));

        zip_file_t* entry_file = zip_fopen_index(archive, entry.index, 0);
        if (entry_file == nullptr)
        {
            DAS_CORE_LOG_WARN(
                "ZIP entry open failed: {}",
                zip_strerror(archive));
            return DAS_E_INVALID_ARGUMENT;
        }

        const auto result = ReadZipEntryToSink(
            entry,
            entry_file,
            [&out](const char* data, zip_uint64_t size)
            { out.append(data, static_cast<std::size_t>(size)); });
        zip_fclose(entry_file);
        return result;
    }

    // 单趟条目快照：条目计数失败 / 空档 / 单条目 stat 失败统一按
    // nullopt 返回（两个入口共用，调用方 fail-closed 拒绝）。
    std::optional<std::vector<EntryInfo>> SnapshotEntries(zip_t* archive)
    {
        const zip_int64_t num_entries = zip_get_num_entries(archive, 0);
        if (num_entries < 0)
        {
            DAS_CORE_LOG_WARN(
                "ZIP entry count failed: {}",
                zip_strerror(archive));
            return std::nullopt;
        }
        if (num_entries == 0)
        {
            DAS_CORE_LOG_WARN("ZIP archive is empty");
            return std::nullopt;
        }

        std::vector<EntryInfo> entries;
        entries.reserve(static_cast<std::size_t>(num_entries));
        for (zip_int64_t i = 0; i < num_entries; ++i)
        {
            zip_stat_t stat;
            zip_stat_init(&stat);
            if (zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &stat)
                < 0)
            {
                DAS_CORE_LOG_WARN("ZIP stat failed: {}", zip_strerror(archive));
                return std::nullopt;
            }

            EntryInfo info;
            info.index = static_cast<zip_uint64_t>(i);
            info.name = (stat.valid & ZIP_STAT_NAME) != 0 ? stat.name : "";
            info.is_directory = !info.name.empty() && info.name.back() == '/';
            info.size_valid = (stat.valid & ZIP_STAT_SIZE) != 0;
            info.declared_size = stat.size;
            entries.push_back(std::move(info));
        }
        return entries;
    }

    // flat 包判定：任一文件条目名含 '/' 即视为带子目录的 packaged 包。
    bool HasSubdirectory(const std::vector<EntryInfo>& entries)
    {
        for (const auto& e : entries)
        {
            if (!e.is_directory && e.name.find('/') != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    // Create a unique temp directory under parent
    std::filesystem::path CreateTempDir(const std::filesystem::path& parent)
    {
        std::random_device                      rd;
        std::uniform_int_distribution<unsigned> dist;

        for (int attempt = 0; attempt < 8; ++attempt)
        {
            auto            name = fmt::format(".tmp_install_{:08x}", dist(rd));
            auto            path = parent / name;
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            if (!ec)
            {
                return path;
            }
        }

        DAS_CORE_LOG_WARN(
            "Failed to create temp directory under {}",
            DAS::Utils::U8AsString(parent.u8string()));
        return {};
    }

    // RAII guard that removes a directory on scope exit
    struct TempDirGuard
    {
        std::filesystem::path path;
        bool                  dismiss = false;

        ~TempDirGuard()
        {
            if (!dismiss && !path.empty())
            {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        }
    };

} // anonymous namespace

DasResult InstallPlugin(
    const std::filesystem::path& plugin_dir,
    std::string_view             zip_data)
{
    if (zip_data.empty())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    if (!std::filesystem::exists(plugin_dir))
    {
        std::error_code ec;
        std::filesystem::create_directories(plugin_dir, ec);
        if (ec)
        {
            DAS_CORE_LOG_WARN(
                "Failed to create plugin directory: {}",
                DAS::Utils::U8AsString(plugin_dir.u8string()));
            return DAS_E_FAIL;
        }
    }

    // --- Step 1: Open archive and snapshot entries (fail-closed) ---
    zip_t* archive = OpenZipArchiveFromMemory(zip_data);
    if (archive == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    const auto snapshot = SnapshotEntries(archive);
    if (!snapshot)
    {
        zip_discard(archive);
        return DAS_E_INVALID_ARGUMENT;
    }
    const auto& entries = *snapshot;

    // 整档 fail-closed：任一条目名不安全即拒绝整个安装（磁盘零写入）。
    for (const auto& e : entries)
    {
        if (!IsFilenameSafe(e.name))
        {
            DAS_CORE_LOG_WARN("ZIP entry has unsafe filename: {}", e.name);
            zip_discard(archive);
            return DAS_E_INVALID_ARGUMENT;
        }
    }

    // --- Step 2: Create temp directory for atomic extraction ---
    auto temp_dir = CreateTempDir(plugin_dir);
    if (temp_dir.empty())
    {
        zip_discard(archive);
        return DAS_E_FAIL;
    }

    TempDirGuard temp_guard{temp_dir};

    // --- Step 3: Detect flat ZIP, compute effective filenames ---
    const bool has_subdir = HasSubdirectory(entries);

    std::string auto_prefix;
    if (!has_subdir)
    {
        // flat 包自动包装目录名派生（与 PluginScanner 的发现约定对齐）。
        // 候选顺序：根级 manifest.json 优先，其余根级 .json 按条目序。
        // 候选有效 = 可读（16MiB 内存上限）+ 可解析为 JSON 对象 + 含
        // "name" 或 "guid" 字符串字段（manifest 判别式；无这两者的干扰
        // config.json 跳过）。
        // 目录名规则：manifest.json 候选取 "name" 字段（legacy 兼容，
        // 空 / 不安全按候选无效续试）；其余 X.json 候选取文件主干 X，
        // 保证安装布局 X/X.json 命中扫描器主约定 <目录名>.json，而非
        // name 字段。
        std::vector<const EntryInfo*> candidates;
        for (const auto& e : entries)
        {
            if (!e.is_directory && e.name == "manifest.json")
            {
                candidates.push_back(&e);
                break;
            }
        }
        for (const auto& e : entries)
        {
            if (!e.is_directory && e.name != "manifest.json"
                && e.name.size() >= 5
                && e.name.compare(e.name.size() - 5, 5, ".json") == 0
                && e.name.find('/') == std::string::npos)
            {
                candidates.push_back(&e);
            }
        }

        for (const auto* candidate : candidates)
        {
            std::string file_data;
            if (ReadEntryToString(
                    archive,
                    *candidate,
                    file_data,
                    MAX_MANIFEST_ENTRY_BYTES)
                != DAS_S_OK)
            {
                continue;
            }

            try
            {
                auto parsed = Das::Utils::ParseYyjsonFromString(file_data);
                if (!parsed)
                {
                    continue;
                }
                auto obj = parsed->as_object();
                if (!obj)
                {
                    continue;
                }

                auto name_val = (*obj)[std::string_view("name")];
                auto name_opt = name_val.as_string();
                auto guid_val = (*obj)[std::string_view("guid")];
                auto guid_opt = guid_val.as_string();

                // manifest 判别式：name / guid 字符串字段皆缺的候选跳过。
                if (!name_opt && !guid_opt)
                {
                    continue;
                }

                std::string dir_name;
                if (candidate->name == "manifest.json")
                {
                    dir_name = name_opt ? std::string(*name_opt) : "";
                    if (!IsFilenameSafe(dir_name))
                    {
                        continue;
                    }
                }
                else
                {
                    dir_name =
                        candidate->name.substr(0, candidate->name.size() - 5);
                    if (!IsSinglePathComponentSafe(dir_name))
                    {
                        continue;
                    }
                }

                auto_prefix = dir_name + "/";
                break;
            }
            catch (const std::exception&)
            {
            }
        }

        if (auto_prefix.empty())
        {
            DAS_CORE_LOG_WARN(
                "Flat ZIP has no usable manifest candidate for wrap directory "
                "name");
            zip_discard(archive);
            return DAS_E_INVALID_ARGUMENT;
        }
    }

    // --- Step 4: Extract each entry to temp dir ---
    for (const auto& entry : entries)
    {
        auto effective_name = auto_prefix + entry.name;
        auto target_path = temp_dir / Das::Utils::PathFromUtf8(effective_name);

        if (!IsPathSafe(temp_dir, target_path))
        {
            DAS_CORE_LOG_WARN(
                "ZIP entry escapes target directory: {}",
                effective_name);
            zip_discard(archive);
            return DAS_E_INVALID_ARGUMENT;
        }

        if (entry.is_directory)
        {
            std::error_code ec;
            std::filesystem::create_directories(target_path, ec);
            if (ec)
            {
                DAS_CORE_LOG_WARN(
                    "Failed to create directory: {}",
                    DAS::Utils::U8AsString(target_path.u8string()));
                zip_discard(archive);
                return DAS_E_FAIL;
            }
            continue;
        }

        auto result = ReadEntryToFile(archive, entry, target_path);
        if (result != DAS_S_OK)
        {
            zip_discard(archive);
            return result;
        }
    }

    if (!CloseZipArchive(archive))
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // --- Step 5: Validate manifest in temp dir ---
    bool            found_manifest = false;
    std::string     plugin_name;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(
             temp_dir,
             std::filesystem::directory_options::skip_permission_denied,
             ec))
    {
        if (!entry.is_directory())
        {
            continue;
        }

        auto manifest = FindManifest(entry.path());
        if (!manifest.empty())
        {
            found_manifest = true;
            plugin_name = std::string{
                DAS::Utils::U8AsString(entry.path().filename().u8string())};
            break;
        }
    }

    if (!found_manifest)
    {
        DAS_CORE_LOG_WARN("ZIP does not contain a valid plugin manifest");
        return DAS_E_FAIL;
    }

    // --- Step 6: Atomic install — rename temp into final location ---
    auto src = temp_dir / Das::Utils::PathFromUtf8(plugin_name);
    auto dst = plugin_dir / Das::Utils::PathFromUtf8(plugin_name);
    auto old = plugin_dir / Das::Utils::PathFromUtf8(plugin_name + ".old");

    // Move existing plugin aside
    if (std::filesystem::exists(dst))
    {
        std::filesystem::rename(dst, old, ec);
        if (ec)
        {
            DAS_CORE_LOG_WARN(
                "Failed to rename existing plugin: {}",
                DAS::Utils::U8AsString(dst.u8string()));
            return DAS_E_FAIL;
        }
    }

    // Move new plugin into place
    std::filesystem::rename(src, dst, ec);
    if (ec)
    {
        DAS_CORE_LOG_WARN(
            "Failed to install plugin: {} -> {}",
            DAS::Utils::U8AsString(src.u8string()),
            DAS::Utils::U8AsString(dst.u8string()));
        // Restore old plugin
        if (std::filesystem::exists(old))
        {
            std::filesystem::rename(old, dst, ec);
            if (ec)
            {
                // 恢复失败：既有插件已不在位，.old 目录是唯一副本，留痕
                // 供人工恢复。
                DAS_CORE_LOG_ERROR(
                    "Failed to restore previous plugin version: {} -> {}",
                    DAS::Utils::U8AsString(old.u8string()),
                    DAS::Utils::U8AsString(dst.u8string()));
            }
        }
        return DAS_E_FAIL;
    }

    // Cleanup: remove old version and temp dir
    if (std::filesystem::exists(old))
    {
        std::filesystem::remove_all(old, ec);
        if (ec)
        {
            DAS_CORE_LOG_WARN(
                "Failed to remove old plugin version: {}",
                DAS::Utils::U8AsString(old.u8string()));
        }
    }

    temp_guard.dismiss = true;
    std::filesystem::remove_all(temp_dir, ec);
    if (ec)
    {
        DAS_CORE_LOG_WARN(
            "Failed to remove temp install directory: {}",
            DAS::Utils::U8AsString(temp_dir.u8string()));
    }

    return DAS_S_OK;
}

DasResult ReadPluginManifestMetadataFromZip(
    std::string_view zip_data,
    std::string&     out_guid,
    std::string&     out_name)
{
    out_guid.clear();
    out_name.clear();

    if (zip_data.empty())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    zip_t* archive = OpenZipArchiveFromMemory(zip_data);
    if (archive == nullptr)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // 单趟条目快照（无名条目不可能命中 manifest 候选规则，天然被后续
    // 筛选跳过）。
    const auto snapshot = SnapshotEntries(archive);
    if (!snapshot)
    {
        zip_discard(archive);
        return DAS_E_INVALID_ARGUMENT;
    }
    const auto& entries = *snapshot;

    // Find manifest entry:
    // For flat ZIPs (no subdirs), any root .json entry is a candidate.
    // For packaged ZIPs (with subdirs), look for */manifest.json only.
    const bool has_subdir = HasSubdirectory(entries);

    // Candidate manifest entries to try
    std::vector<const EntryInfo*> candidates;

    if (!has_subdir)
    {
        // Flat ZIP: any root .json file is a candidate
        for (const auto& e : entries)
        {
            if (!e.is_directory && e.name.size() >= 5
                && e.name.compare(e.name.size() - 5, 5, ".json") == 0
                && e.name.find('/') == std::string::npos)
            {
                candidates.push_back(&e);
            }
        }
    }
    else
    {
        // Packaged ZIP: look for manifest.json at root or */manifest.json.
        for (const auto& e : entries)
        {
            if (e.is_directory)
            {
                continue;
            }
            if (e.name == "manifest.json"
                || e.name.find("/manifest.json") != std::string::npos)
            {
                candidates.push_back(&e);
            }
        }
    }

    if (candidates.empty())
    {
        DAS_CORE_LOG_WARN(
            "ReadPluginManifestMetadataFromZip: no manifest candidate found");
        zip_discard(archive);
        return DAS_E_INVALID_ARGUMENT;
    }

    // 逐候选读取：声明大小无效或超 manifest 内存上限的候选在开读前
    // 被拒（读串函数入口检查，WARN 含候选名）；读取失败（含 CRC 击穿）
    // 同路 continue——候选中途不关包。
    for (const auto* candidate : candidates)
    {
        std::string file_data;
        if (ReadEntryToString(
                archive,
                *candidate,
                file_data,
                MAX_MANIFEST_ENTRY_BYTES)
            != DAS_S_OK)
        {
            continue;
        }

        try
        {
            auto parsed = Das::Utils::ParseYyjsonFromString(file_data);
            if (!parsed)
            {
                DAS_CORE_LOG_WARN(
                    "ReadPluginManifestMetadataFromZip: JSON parse failed for "
                    "{}",
                    candidate->name);
                continue;
            }

            auto obj = parsed->as_object();
            if (!obj)
            {
                DAS_CORE_LOG_WARN(
                    "ReadPluginManifestMetadataFromZip: manifest is not an "
                    "object for {}",
                    candidate->name);
                continue;
            }

            auto guid_val = (*obj)[std::string_view("guid")];
            if (!guid_val.is_null() && guid_val.is_string())
            {
                auto guid_opt = guid_val.as_string();
                if (guid_opt)
                {
                    out_guid = std::string(*guid_opt);
                }
            }
            auto name_val = (*obj)[std::string_view("name")];
            if (!name_val.is_null() && name_val.is_string())
            {
                auto name_opt = name_val.as_string();
                if (name_opt)
                {
                    out_name = std::string(*name_opt);
                }
            }

            if (!out_guid.empty() && !out_name.empty())
            {
                CloseZipArchive(archive);
                return DAS_S_OK;
            }
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_WARN(
                "ReadPluginManifestMetadataFromZip: JSON parse failed for "
                "{}: {}",
                candidate->name,
                e.what());
            continue;
        }
    }

    // If we found at least a guid or name from partial results, that's OK
    if (!out_guid.empty() || !out_name.empty())
    {
        CloseZipArchive(archive);
        return DAS_S_OK;
    }

    DAS_CORE_LOG_WARN(
        "ReadPluginManifestMetadataFromZip: no manifest with guid/name found");
    zip_discard(archive);
    return DAS_E_INVALID_ARGUMENT;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
