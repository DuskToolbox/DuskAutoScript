#include <das/Core/ForeignInterfaceHost/PluginZipExtractor.h>

#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/Logger/Logger.h>

#include <array>
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

#pragma pack(push, 1)
    struct LocalFileHeader
    {
        uint32_t signature;
        uint16_t version_needed;
        uint16_t flags;
        uint16_t compression;
        uint16_t mod_time;
        uint16_t mod_date;
        uint32_t crc32;
        uint32_t compressed_size;
        uint32_t uncompressed_size;
        uint16_t filename_length;
        uint16_t extra_field_length;
    };

    struct CentralDirectoryEntry
    {
        uint32_t signature;
        uint16_t version_made_by;
        uint16_t version_needed;
        uint16_t flags;
        uint16_t compression;
        uint16_t mod_time;
        uint16_t mod_date;
        uint32_t crc32;
        uint32_t compressed_size;
        uint32_t uncompressed_size;
        uint16_t filename_length;
        uint16_t extra_field_length;
        uint16_t comment_length;
        uint16_t disk_start;
        uint16_t internal_attr;
        uint32_t external_attr;
        uint32_t local_header_offset;
    };

    struct EndOfCentralDirRecord
    {
        uint32_t signature;
        uint16_t disk_number;
        uint16_t disk_with_cd;
        uint16_t num_entries_on_disk;
        uint16_t num_entries;
        uint32_t cd_size;
        uint32_t cd_offset;
        uint16_t comment_length;
    };
#pragma pack(pop)

    // Helper: read little-endian values from byte buffer
    template <typename T>
    T ReadLE(const char* data, size_t offset)
    {
        T value;
        std::memcpy(&value, data + offset, sizeof(T));
        return value;
    }

    bool FindEndOfCentralDir(const std::string& zip_data, size_t& eocd_offset)
    {
        // EOCD is at most 22 + 65535 bytes from the end
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

    // Decompress Deflate data using zlib
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

        // -MAX_WBITS tells zlib to expect raw deflate (no zlib/gzip header)
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        {
            return false;
        }

        int ret = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);

        return ret == Z_STREAM_END;
    }

    // Validate that a path does not escape the target directory
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

        // Use lexically_relative to check containment: if the entry is inside
        // target, the relative path will not start with ".."
        auto relative = canonical_entry.lexically_relative(canonical_target);
        auto rel_str = relative.string();
        if (rel_str.size() >= 2 && rel_str.substr(0, 2) == "..")
        {
            return false;
        }
        if (rel_str == ".")
        {
            // entry_path is exactly target_dir
            return false;
        }
        return !rel_str.empty();
    }

    // Check filename for path traversal patterns
    bool IsFilenameSafe(const std::string& filename)
    {
        if (filename.empty())
        {
            return false;
        }

        // Must not start with '/' or '\'
        if (filename[0] == '/' || filename[0] == '\\')
        {
            return false;
        }

        // Must not contain ".." path component
        std::istringstream ss(filename);
        std::string        component;
        while (std::getline(ss, component, '/'))
        {
            if (component == "..")
            {
                return false;
            }
        }

        // Also check backslash separators
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

    struct ExtractedEntry
    {
        std::string filename;
        std::string data;
        bool        is_directory;
    };

    DasResult ParseAndExtractZip(
        const std::string&           zip_data,
        std::vector<ExtractedEntry>& entries)
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
            if (pos + sizeof(CentralDirectoryEntry) > zip_data.size())
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

            std::string filename(zip_data.data() + pos + 46, filename_length);

            pos += 46 + filename_length + extra_length + comment_length;

            // Validate filename
            if (!IsFilenameSafe(filename))
            {
                DAS_CORE_LOG_WARN(
                    "ZIP entry has unsafe filename: {}",
                    filename);
                return DAS_E_INVALID_ARGUMENT;
            }

            bool is_directory = !filename.empty() && filename.back() == '/';

            // Read from local file header to get actual data
            if (local_header_offset + sizeof(LocalFileHeader) > zip_data.size())
            {
                DAS_CORE_LOG_WARN("Invalid ZIP: truncated local file header");
                return DAS_E_INVALID_ARGUMENT;
            }

            auto local_sig =
                ReadLE<uint32_t>(zip_data.data(), local_header_offset);
            if (local_sig != kLocalFileHeaderSig)
            {
                DAS_CORE_LOG_WARN(
                    "Invalid ZIP: bad local file header signature");
                return DAS_E_INVALID_ARGUMENT;
            }

            auto local_filename_length =
                ReadLE<uint16_t>(zip_data.data(), local_header_offset + 26);
            auto local_extra_length =
                ReadLE<uint16_t>(zip_data.data(), local_header_offset + 28);

            size_t data_offset = local_header_offset + 30
                                 + local_filename_length + local_extra_length;

            ExtractedEntry entry;
            entry.filename = filename;
            entry.is_directory = is_directory;

            if (!is_directory && compressed_size > 0)
            {
                if (data_offset + compressed_size > zip_data.size())
                {
                    DAS_CORE_LOG_WARN(
                        "Invalid ZIP: truncated file data for {}",
                        filename);
                    return DAS_E_INVALID_ARGUMENT;
                }

                if (compression == kCompressionStored)
                {
                    entry.data.assign(
                        zip_data.data() + data_offset,
                        compressed_size);
                }
                else if (compression == kCompressionDeflate)
                {
                    std::string compressed(
                        zip_data.data() + data_offset,
                        compressed_size);
                    if (!InflateData(
                            compressed,
                            compressed_size,
                            entry.data,
                            uncompressed_size))
                    {
                        DAS_CORE_LOG_WARN(
                            "ZIP decompression failed for {}",
                            filename);
                        return DAS_E_INVALID_ARGUMENT;
                    }
                }
                else
                {
                    DAS_CORE_LOG_WARN(
                        "ZIP entry uses unsupported compression {}: {}",
                        compression,
                        filename);
                    return DAS_E_INVALID_ARGUMENT;
                }
            }

            entries.push_back(std::move(entry));
        }

        return DAS_S_OK;
    }

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
                plugin_dir.string());
            return DAS_E_FAIL;
        }
    }

    // Parse and extract ZIP entries in memory
    std::string                 zip_str(zip_data.data(), zip_data.size());
    std::vector<ExtractedEntry> entries;

    auto result = ParseAndExtractZip(zip_str, entries);
    if (result != DAS_S_OK)
    {
        return result;
    }

    if (entries.empty())
    {
        DAS_CORE_LOG_WARN("ZIP archive is empty");
        return DAS_E_INVALID_ARGUMENT;
    }

    // Detect flat ZIP (no subdirectory wrapping) and auto-wrap into a
    // plugin subdirectory. Flat ZIP means all filenames contain no '/' or
    // '\' separator.
    bool has_subdir = false;
    for (const auto& e : entries)
    {
        if (!e.is_directory && e.filename.find('/') != std::string::npos)
        {
            has_subdir = true;
            break;
        }
    }

    // If flat ZIP, parse manifest to get plugin name, then prefix all
    // entries with "<name>/"
    if (!has_subdir)
    {
        std::string plugin_name;
        for (const auto& e : entries)
        {
            if (e.is_directory)
            {
                continue;
            }
            if (e.filename.size() >= 5
                && e.filename.compare(e.filename.size() - 5, 5, ".json") == 0)
            {
                try
                {
                    auto json_data = nlohmann::json::parse(e.data);
                    if (json_data.contains("name"))
                    {
                        plugin_name = json_data["name"].get<std::string>();
                    }
                }
                catch (const std::exception&)
                {
                }
                break;
            }
        }

        if (plugin_name.empty())
        {
            DAS_CORE_LOG_WARN("Flat ZIP has no manifest with 'name' field");
            return DAS_E_INVALID_ARGUMENT;
        }

        for (auto& e : entries)
        {
            e.filename = plugin_name + "/" + e.filename;
        }
    }

    // Write extracted files to plugin_dir
    for (const auto& entry : entries)
    {
        auto target_path = plugin_dir / entry.filename;

        if (!IsPathSafe(plugin_dir, target_path))
        {
            DAS_CORE_LOG_WARN(
                "ZIP entry escapes target directory: {}",
                entry.filename);
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
                    target_path.string());
                return DAS_E_FAIL;
            }
        }
        else
        {
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
                    target_path.string());
                return DAS_E_FAIL;
            }
            ofs.write(entry.data.data(), entry.data.size());
            if (!ofs)
            {
                DAS_CORE_LOG_WARN(
                    "Failed to write file data: {}",
                    target_path.string());
                return DAS_E_FAIL;
            }
        }
    }

    // Verify at least one valid manifest exists
    bool            found_manifest = false;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(
             plugin_dir,
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
            break;
        }
    }

    if (!found_manifest)
    {
        DAS_CORE_LOG_WARN("ZIP does not contain a valid plugin manifest");

        // Clean up extracted entries in reverse order (files first, then
        // dirs)
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        {
            auto            target = plugin_dir / it->filename;
            std::error_code ec;
            if (it->is_directory)
            {
                std::filesystem::remove_all(target, ec);
            }
            else
            {
                std::filesystem::remove(target, ec);
            }
        }

        return DAS_E_FAIL;
    }

    return DAS_S_OK;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
