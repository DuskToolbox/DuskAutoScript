#include <das/Core/ForeignInterfaceHost/PluginZipExtractor.h>

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <zip.h>
#include <zlib.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    // ZIP format signatures
    constexpr uint32_t LOCAL_FILE_HEADER_SIG = 0x04034B50;
    constexpr uint32_t CENTRAL_DIR_SIG = 0x02014B50;
    constexpr uint32_t END_OF_CENTRAL_DIR_SIG = 0x06054B50;
    constexpr uint16_t COMPRESSION_STORED = 0;
    constexpr uint16_t COMPRESSION_DEFLATE = 8;
    constexpr size_t   END_OF_CENTRAL_DIR_SIZE = 22;
    constexpr size_t   MAX_COMMENT_SIZE = 65535;

    // Read little-endian values from byte buffer
    template <typename T>
    T ReadLE(const char* data, size_t offset)
    {
        T value;
        std::memcpy(&value, data + offset, sizeof(T));
        return value;
    }

    bool FindEndOfCentralDir(const std::string& zip_data, size_t& eocd_offset)
    {
        // 短于 EOCD 记录的输入直接判无效：更短的输入会让下方回扫的
        // 下标运算下溢，读到缓冲区之外（不可信输入的越界读）。
        if (zip_data.size() < END_OF_CENTRAL_DIR_SIZE)
        {
            return false;
        }

        const size_t max_search = std::min(
            zip_data.size(),
            END_OF_CENTRAL_DIR_SIZE + MAX_COMMENT_SIZE);

        for (size_t i = 0; i < max_search - END_OF_CENTRAL_DIR_SIZE + 1; ++i)
        {
            const size_t offset = zip_data.size() - END_OF_CENTRAL_DIR_SIZE - i;
            if (ReadLE<uint32_t>(zip_data.data(), offset)
                == END_OF_CENTRAL_DIR_SIG)
            {
                eocd_offset = offset;
                return true;
            }
        }

        return false;
    }

    // Decompress raw Deflate using zlib
    bool InflateData(
        const std::string& compressed,
        size_t             compressed_size,
        std::string&       output,
        size_t             uncompressed_size)
    {
        output.resize(uncompressed_size);

        z_stream stream{};
        stream.next_in =
            reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
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

        std::istringstream ss(filename);
        std::string        component;
        while (std::getline(ss, component, '/'))
        {
            if (component == "..")
            {
                return false;
            }
        }

        std::istringstream ss2(filename);
        while (std::getline(ss2, component, '\\'))
        {
            if (component == "..")
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

    // UTF-8 std::string 到 path 的唯一物化入口：1:1 字节拷入
    // std::u8string 后走 C++20 path(std::u8string) 构造（u8path 自由
    // 函数已弃用；Windows 窄构造走 ANSI 代码页，非 ASCII 名会损坏）。
    std::filesystem::path PathFromUtf8(std::string_view utf8_text)
    {
        std::u8string u8_text(utf8_text.begin(), utf8_text.end());
        return std::filesystem::path{std::move(u8_text)};
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

    // ZIP entry metadata — no file data, extracted directly to disk
    struct ZipEntryMeta
    {
        std::string filename;
        uint16_t    compression;
        uint32_t    compressed_size;
        uint32_t    uncompressed_size;
        uint32_t    local_header_offset;
        bool        is_directory;
    };

    // Parse Central Directory to enumerate entries (metadata only)
    DasResult EnumerateZipEntries(
        const std::string&         zip_data,
        std::vector<ZipEntryMeta>& entries)
    {
        size_t eocd_offset = 0;
        if (!FindEndOfCentralDir(zip_data, eocd_offset))
        {
            DAS_CORE_LOG_WARN(
                "Invalid ZIP: End of Central Directory not found");
            return DAS_E_INVALID_ARGUMENT;
        }

        auto num_entries = ReadLE<uint16_t>(zip_data.data(), eocd_offset + 10);
        auto cd_offset = ReadLE<uint32_t>(zip_data.data(), eocd_offset + 16);

        size_t pos = cd_offset;
        for (uint16_t i = 0; i < num_entries; ++i)
        {
            if (pos + 46 > zip_data.size())
            {
                DAS_CORE_LOG_WARN("Invalid ZIP: truncated central directory");
                return DAS_E_INVALID_ARGUMENT;
            }

            auto sig = ReadLE<uint32_t>(zip_data.data(), pos);
            if (sig != CENTRAL_DIR_SIG)
            {
                DAS_CORE_LOG_WARN(
                    "Invalid ZIP: bad central directory signature");
                return DAS_E_INVALID_ARGUMENT;
            }

            auto compression = ReadLE<uint16_t>(zip_data.data(), pos + 10);
            auto compressed_size = ReadLE<uint32_t>(zip_data.data(), pos + 20);
            auto uncompressed_size =
                ReadLE<uint32_t>(zip_data.data(), pos + 24);
            auto filename_length = ReadLE<uint16_t>(zip_data.data(), pos + 28);
            auto extra_length = ReadLE<uint16_t>(zip_data.data(), pos + 30);
            auto comment_length = ReadLE<uint16_t>(zip_data.data(), pos + 32);
            auto local_header_offset =
                ReadLE<uint32_t>(zip_data.data(), pos + 42);

            if (pos + 46 + filename_length + extra_length + comment_length
                > zip_data.size())
            {
                DAS_CORE_LOG_WARN("Invalid ZIP: truncated entry metadata");
                return DAS_E_INVALID_ARGUMENT;
            }

            std::string filename(zip_data.data() + pos + 46, filename_length);

            pos += 46 + filename_length + extra_length + comment_length;

            if (!IsFilenameSafe(filename))
            {
                DAS_CORE_LOG_WARN(
                    "ZIP entry has unsafe filename: {}",
                    filename);
                return DAS_E_INVALID_ARGUMENT;
            }

            ZipEntryMeta meta;
            meta.filename = filename;
            meta.compression = compression;
            meta.compressed_size = compressed_size;
            meta.uncompressed_size = uncompressed_size;
            meta.local_header_offset = local_header_offset;
            meta.is_directory = !filename.empty() && filename.back() == '/';

            entries.push_back(std::move(meta));
        }

        return DAS_S_OK;
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

    const zip_int64_t num_entries = zip_get_num_entries(archive, 0);
    if (num_entries < 0)
    {
        DAS_CORE_LOG_WARN("ZIP entry count failed: {}", zip_strerror(archive));
        zip_discard(archive);
        return DAS_E_INVALID_ARGUMENT;
    }

    if (num_entries == 0)
    {
        DAS_CORE_LOG_WARN("ZIP archive is empty");
        zip_discard(archive);
        return DAS_E_INVALID_ARGUMENT;
    }

    std::vector<EntryInfo> entries;
    entries.reserve(static_cast<std::size_t>(num_entries));
    for (zip_int64_t i = 0; i < num_entries; ++i)
    {
        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &stat) < 0)
        {
            DAS_CORE_LOG_WARN("ZIP stat failed: {}", zip_strerror(archive));
            zip_discard(archive);
            return DAS_E_INVALID_ARGUMENT;
        }

        EntryInfo info;
        info.index = static_cast<zip_uint64_t>(i);
        info.name = (stat.valid & ZIP_STAT_NAME) != 0 ? stat.name : "";
        info.is_directory = !info.name.empty() && info.name.back() == '/';
        info.size_valid = (stat.valid & ZIP_STAT_SIZE) != 0;
        info.declared_size = stat.size;
        entries.push_back(std::move(info));
    }

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
    bool has_subdir = false;
    for (const auto& e : entries)
    {
        if (!e.is_directory && e.name.find('/') != std::string::npos)
        {
            has_subdir = true;
            break;
        }
    }

    std::string auto_prefix;
    if (!has_subdir)
    {
        // Need to find manifest entry to get plugin name for auto-wrap.
        // Read only the first root .json entry (bounded in-memory read).
        for (const auto& e : entries)
        {
            if (e.is_directory)
            {
                continue;
            }

            if (e.name.size() >= 5
                && e.name.compare(e.name.size() - 5, 5, ".json") == 0)
            {
                std::string file_data;
                if (ReadEntryToString(
                        archive,
                        e,
                        file_data,
                        MAX_MANIFEST_ENTRY_BYTES)
                    == DAS_S_OK)
                {
                    try
                    {
                        auto parsed =
                            Das::Utils::ParseYyjsonFromString(file_data);
                        if (parsed)
                        {
                            auto obj = parsed->as_object();
                            if (obj)
                            {
                                auto name_val =
                                    (*obj)[std::string_view("name")];
                                auto name_opt = name_val.as_string();
                                if (name_opt)
                                {
                                    auto_prefix = std::string(*name_opt) + "/";
                                }
                            }
                        }
                    }
                    catch (const std::exception&)
                    {
                    }
                }
                break;
            }
        }

        if (auto_prefix.empty())
        {
            DAS_CORE_LOG_WARN("Flat ZIP has no manifest with 'name' field");
            zip_discard(archive);
            return DAS_E_INVALID_ARGUMENT;
        }
    }

    // --- Step 4: Extract each entry to temp dir ---
    for (const auto& entry : entries)
    {
        auto effective_name = auto_prefix + entry.name;
        auto target_path = temp_dir / PathFromUtf8(effective_name);

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

    if (zip_close(archive) != 0)
    {
        DAS_CORE_LOG_WARN("ZIP close failed: {}", zip_strerror(archive));
        zip_discard(archive);
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
    auto src = temp_dir / PathFromUtf8(plugin_name);
    auto dst = plugin_dir / PathFromUtf8(plugin_name);
    auto old = plugin_dir / PathFromUtf8(plugin_name + ".old");

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
        }
        return DAS_E_FAIL;
    }

    // Cleanup: remove old version and temp dir
    if (std::filesystem::exists(old))
    {
        std::filesystem::remove_all(old, ec);
    }

    temp_guard.dismiss = true;
    std::filesystem::remove_all(temp_dir, ec);

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

    std::string               zip_str(zip_data.data(), zip_data.size());
    std::vector<ZipEntryMeta> entries;
    auto                      result = EnumerateZipEntries(zip_str, entries);
    if (result != DAS_S_OK)
    {
        return result;
    }

    if (entries.empty())
    {
        DAS_CORE_LOG_WARN(
            "ReadPluginManifestMetadataFromZip: ZIP archive is empty");
        return DAS_E_INVALID_ARGUMENT;
    }

    // Find manifest entry — same logic as InstallPlugin:
    // For flat ZIPs (no subdirs), look for a .json entry at root.
    // For packaged ZIPs (with subdirs), look for */manifest.json.
    bool has_subdir = false;
    for (const auto& e : entries)
    {
        if (!e.is_directory && e.filename.find('/') != std::string::npos)
        {
            has_subdir = true;
            break;
        }
    }

    // Candidate manifest entries to try
    std::vector<const ZipEntryMeta*> candidates;

    if (!has_subdir)
    {
        // Flat ZIP: any root .json file is a candidate
        for (const auto& e : entries)
        {
            if (!e.is_directory && e.filename.size() >= 5
                && e.filename.compare(e.filename.size() - 5, 5, ".json") == 0
                && e.filename.find('/') == std::string::npos)
            {
                candidates.push_back(&e);
            }
        }
    }
    else
    {
        // Packaged ZIP: look for */manifest.json or */{dirname}.json
        for (const auto& e : entries)
        {
            if (e.is_directory)
            {
                continue;
            }
            if (e.filename == "manifest.json"
                || e.filename.find("/manifest.json") != std::string::npos)
            {
                candidates.push_back(&e);
            }
        }
    }

    if (candidates.empty())
    {
        DAS_CORE_LOG_WARN(
            "ReadPluginManifestMetadataFromZip: no manifest candidate found");
        return DAS_E_INVALID_ARGUMENT;
    }

    // Try each candidate until one has guid and name
    for (const auto* meta : candidates)
    {
        // Read entry data from memory using local header offset
        auto local_header = meta->local_header_offset;
        if (local_header + 30 > zip_str.size())
        {
            continue;
        }

        auto local_sig = ReadLE<uint32_t>(zip_str.data(), local_header);
        if (local_sig != LOCAL_FILE_HEADER_SIG)
        {
            continue;
        }

        auto lfn_len = ReadLE<uint16_t>(zip_str.data(), local_header + 26);
        auto lex_len = ReadLE<uint16_t>(zip_str.data(), local_header + 28);
        const size_t data_off = local_header + 30 + lfn_len + lex_len;

        if (data_off + meta->compressed_size > zip_str.size())
        {
            continue;
        }

        std::string file_data;
        if (meta->compression == COMPRESSION_STORED)
        {
            file_data.assign(zip_str.data() + data_off, meta->compressed_size);
        }
        else if (meta->compression == COMPRESSION_DEFLATE)
        {
            const std::string compressed(
                zip_str.data() + data_off,
                meta->compressed_size);
            if (!InflateData(
                    compressed,
                    meta->compressed_size,
                    file_data,
                    meta->uncompressed_size))
            {
                DAS_CORE_LOG_WARN(
                    "ReadPluginManifestMetadataFromZip: decompression failed "
                    "for {}",
                    meta->filename);
                continue;
            }
        }
        else
        {
            DAS_CORE_LOG_WARN(
                "ReadPluginManifestMetadataFromZip: unsupported compression {} "
                "for {}",
                meta->compression,
                meta->filename);
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
                    meta->filename);
                continue;
            }

            auto obj = parsed->as_object();
            if (!obj)
            {
                DAS_CORE_LOG_WARN(
                    "ReadPluginManifestMetadataFromZip: manifest is not an object "
                    "for {}",
                    meta->filename);
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
                return DAS_S_OK;
            }
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_WARN(
                "ReadPluginManifestMetadataFromZip: JSON parse failed for "
                "{}: {}",
                meta->filename,
                e.what());
            continue;
        }
    }

    // If we found at least a guid or name from partial results, that's OK
    if (!out_guid.empty() || !out_name.empty())
    {
        return DAS_S_OK;
    }

    DAS_CORE_LOG_WARN(
        "ReadPluginManifestMetadataFromZip: no manifest with guid/name found");
    return DAS_E_INVALID_ARGUMENT;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
