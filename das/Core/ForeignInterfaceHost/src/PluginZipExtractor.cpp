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
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <zlib.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    // ZIP format signatures
    constexpr uint32_t kLocalFileHeaderSig = 0x04034B50;
    constexpr uint32_t kCentralDirSig = 0x02014B50;
    constexpr uint32_t kEndOfCentralDirSig = 0x06054B50;
    constexpr uint16_t kCompressionStored = 0;
    constexpr uint16_t kCompressionDeflate = 8;
    constexpr size_t   kEndOfCentralDirSize = 22;
    constexpr size_t   kMaxCommentSize = 65535;

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
        size_t max_search =
            std::min(zip_data.size(), kEndOfCentralDirSize + kMaxCommentSize);

        for (size_t i = 0; i < max_search - kEndOfCentralDirSize + 1; ++i)
        {
            size_t offset = zip_data.size() - kEndOfCentralDirSize - i;
            if (ReadLE<uint32_t>(zip_data.data(), offset)
                == kEndOfCentralDirSig)
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

        int ret = inflate(&stream, Z_FINISH);
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
            if (sig != kCentralDirSig)
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

    // Extract a single ZIP entry directly to disk
    DasResult ExtractEntryToDisk(
        const std::string&           zip_data,
        const ZipEntryMeta&          meta,
        const std::filesystem::path& target_path)
    {
        if (meta.is_directory)
        {
            std::error_code ec;
            std::filesystem::create_directories(target_path, ec);
            if (ec)
            {
                DAS_CORE_LOG_WARN(
                    "Failed to create directory: {}",
                    DAS::Utils::U8AsString(target_path.u8string()));
                return DAS_E_FAIL;
            }
            return DAS_S_OK;
        }

        // Ensure parent directory exists
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

        if (meta.compressed_size == 0)
        {
            return DAS_S_OK;
        }

        // Resolve data offset from Local File Header
        auto local_header = meta.local_header_offset;
        if (local_header + 30 > zip_data.size())
        {
            DAS_CORE_LOG_WARN("Invalid ZIP: truncated local file header");
            return DAS_E_INVALID_ARGUMENT;
        }

        auto local_sig = ReadLE<uint32_t>(zip_data.data(), local_header);
        if (local_sig != kLocalFileHeaderSig)
        {
            DAS_CORE_LOG_WARN("Invalid ZIP: bad local file header signature");
            return DAS_E_INVALID_ARGUMENT;
        }

        auto local_filename_length =
            ReadLE<uint16_t>(zip_data.data(), local_header + 26);
        auto local_extra_length =
            ReadLE<uint16_t>(zip_data.data(), local_header + 28);
        size_t data_offset =
            local_header + 30 + local_filename_length + local_extra_length;

        if (data_offset + meta.compressed_size > zip_data.size())
        {
            DAS_CORE_LOG_WARN(
                "Invalid ZIP: truncated file data for {}",
                meta.filename);
            return DAS_E_INVALID_ARGUMENT;
        }

        if (meta.compression == kCompressionStored)
        {
            ofs.write(zip_data.data() + data_offset, meta.compressed_size);
        }
        else if (meta.compression == kCompressionDeflate)
        {
            std::string compressed(
                zip_data.data() + data_offset,
                meta.compressed_size);
            std::string output;
            if (!InflateData(
                    compressed,
                    meta.compressed_size,
                    output,
                    meta.uncompressed_size))
            {
                DAS_CORE_LOG_WARN(
                    "ZIP decompression failed for {}",
                    meta.filename);
                return DAS_E_INVALID_ARGUMENT;
            }
            ofs.write(output.data(), output.size());
        }
        else
        {
            DAS_CORE_LOG_WARN(
                "ZIP entry uses unsupported compression {}: {}",
                meta.compression,
                meta.filename);
            return DAS_E_INVALID_ARGUMENT;
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

    // --- Phase 1: Enumerate ZIP entries (metadata only, no decompression) ---
    std::string               zip_str(zip_data.data(), zip_data.size());
    std::vector<ZipEntryMeta> entries;

    auto result = EnumerateZipEntries(zip_str, entries);
    if (result != DAS_S_OK)
    {
        return result;
    }

    if (entries.empty())
    {
        DAS_CORE_LOG_WARN("ZIP archive is empty");
        return DAS_E_INVALID_ARGUMENT;
    }

    // --- Phase 2: Create temp directory for atomic extraction ---
    auto temp_dir = CreateTempDir(plugin_dir);
    if (temp_dir.empty())
    {
        return DAS_E_FAIL;
    }

    TempDirGuard temp_guard{temp_dir};

    // --- Phase 3: Detect flat ZIP, compute effective filenames ---
    bool has_subdir = false;
    for (const auto& e : entries)
    {
        if (!e.is_directory && e.filename.find('/') != std::string::npos)
        {
            has_subdir = true;
            break;
        }
    }

    std::string auto_prefix;
    if (!has_subdir)
    {
        // Need to find manifest entry to get plugin name for auto-wrap.
        // Only decompress the manifest .json entry temporarily.
        for (const auto& e : entries)
        {
            if (e.is_directory)
            {
                continue;
            }

            if (e.filename.size() >= 5
                && e.filename.compare(e.filename.size() - 5, 5, ".json") == 0)
            {
                // Read and decompress just this one entry to get plugin name
                auto local_header = e.local_header_offset;
                if (local_header + 30 > zip_str.size())
                {
                    continue;
                }

                auto lfn_len =
                    ReadLE<uint16_t>(zip_str.data(), local_header + 26);
                auto lex_len =
                    ReadLE<uint16_t>(zip_str.data(), local_header + 28);
                size_t data_off = local_header + 30 + lfn_len + lex_len;

                if (data_off + e.compressed_size > zip_str.size())
                {
                    continue;
                }

                std::string file_data;
                if (e.compression == kCompressionStored)
                {
                    file_data.assign(
                        zip_str.data() + data_off,
                        e.compressed_size);
                }
                else if (e.compression == kCompressionDeflate)
                {
                    std::string compressed(
                        zip_str.data() + data_off,
                        e.compressed_size);
                    InflateData(
                        compressed,
                        e.compressed_size,
                        file_data,
                        e.uncompressed_size);
                }

                try
                {
                    auto parsed = Das::Utils::ParseYyjsonFromString(file_data);
                    if (parsed)
                    {
                        auto obj = parsed->as_object();
                        if (obj)
                        {
                            auto name_val = (*obj)[std::string_view("name")];
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
                break;
            }
        }

        if (auto_prefix.empty())
        {
            DAS_CORE_LOG_WARN("Flat ZIP has no manifest with 'name' field");
            return DAS_E_INVALID_ARGUMENT;
        }
    }

    // --- Phase 4: Extract each entry directly to temp dir ---
    for (const auto& meta : entries)
    {
        auto effective_name = auto_prefix + meta.filename;
        auto target_path = temp_dir / effective_name;

        if (!IsPathSafe(temp_dir, target_path))
        {
            DAS_CORE_LOG_WARN(
                "ZIP entry escapes target directory: {}",
                effective_name);
            return DAS_E_INVALID_ARGUMENT;
        }

        ZipEntryMeta effective_meta = meta;
        effective_meta.filename = effective_name;

        result = ExtractEntryToDisk(zip_str, effective_meta, target_path);
        if (result != DAS_S_OK)
        {
            return result;
        }
    }

    // --- Phase 5: Validate manifest in temp dir ---
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

    // --- Phase 6: Atomic install — rename temp into final location ---
    auto src = temp_dir / plugin_name;
    auto dst = plugin_dir / plugin_name;
    auto old = plugin_dir / (plugin_name + ".old");

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
        if (local_sig != kLocalFileHeaderSig)
        {
            continue;
        }

        auto   lfn_len = ReadLE<uint16_t>(zip_str.data(), local_header + 26);
        auto   lex_len = ReadLE<uint16_t>(zip_str.data(), local_header + 28);
        size_t data_off = local_header + 30 + lfn_len + lex_len;

        if (data_off + meta->compressed_size > zip_str.size())
        {
            continue;
        }

        std::string file_data;
        if (meta->compression == kCompressionStored)
        {
            file_data.assign(zip_str.data() + data_off, meta->compressed_size);
        }
        else if (meta->compression == kCompressionDeflate)
        {
            std::string compressed(
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
