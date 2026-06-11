#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINSCANNER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINSCANNER_H

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/DasExport.h>
#include <das/IDasBase.h>

#include <filesystem>
#include <string>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

// File entry returned by directory listing — shared between local and IPC
// providers.
struct FileEntry
{
    std::string name; // file or directory name (e.g. "manifest.json")
    std::string
         absolute_path; // absolute path string (passed to LOAD_PLUGIN V1)
    bool is_directory = false; // true = directory, false = file
};

// ─── Existing non-template API ───

DAS_EXPORT std::vector<PluginPackageDesc> ScanPlugins(
    const std::filesystem::path& plugin_dir);

DAS_EXPORT void CleanupMarkedPlugins(const std::filesystem::path& plugin_dir);

DAS_EXPORT DasResult
MarkForDeletion(const std::filesystem::path& plugin_dir, const DasGuid& guid);

DAS_EXPORT std::filesystem::path FindManifest(
    const std::filesystem::path& plugin_dir_entry);

DAS_EXPORT yyjson::value PluginPackageDescToJson(const PluginPackageDesc& desc);

DAS_EXPORT yyjson::value PluginPackageDescDetailToJson(
    const PluginPackageDesc& desc);

// ─── Inline helper functions ───

// Check whether a file with the given name exists in the entry list.
inline bool HasFileEntry(
    const std::vector<FileEntry>& entries,
    const std::string&            filename)
{
    for (const auto& e : entries)
    {
        if (!e.is_directory && e.name == filename)
        {
            return true;
        }
    }
    return false;
}

// Check whether a directory with the given name exists in the entry list.
inline bool HasDirEntry(
    const std::vector<FileEntry>& entries,
    const std::string&            dirname)
{
    for (const auto& e : entries)
    {
        if (e.is_directory && e.name == dirname)
        {
            return true;
        }
    }
    return false;
}

// Find the manifest file name in a subdirectory's entries.
// Priority: dirname + ".json", fallback: "manifest.json"
inline std::string FindManifestInEntries(
    const std::vector<FileEntry>& subdir_entries,
    const std::string&            dirname)
{
    std::string primary = dirname + ".json";
    for (const auto& e : subdir_entries)
    {
        if (!e.is_directory && e.name == primary)
        {
            return e.name;
        }
    }
    for (const auto& e : subdir_entries)
    {
        if (!e.is_directory && e.name == "manifest.json")
        {
            return e.name;
        }
    }
    return {};
}

// ─── Template scan function ───
//
// Provider duck-typed interface (no base class required):
//
//   struct SomeProvider {
//       std::vector<FileEntry> ListDirectory(
//           const std::string& relative_path_u8, bool recursive);
//       std::string ReadFile(const std::string& relative_path_u8);
//       std::string GetBasePath() const;
//   };
//
// ListDirectory: return entries under the given relative directory
//   (empty string = root of the provider's working directory).
// ReadFile: read a file's raw bytes as a string.
// GetBasePath: return the provider's base working path.

template <typename FileProvider>
std::vector<PluginPackageDesc> ScanPluginsWith(FileProvider& provider)
{
    std::vector<PluginPackageDesc> result;

    // Layer 1: list root directory entries
    auto root_entries = provider.ListDirectory("", false);

    for (const auto& entry : root_entries)
    {
        if (entry.is_directory)
        {
            // ── Directory mode ──
            const auto& dirname = entry.name;

            // Skip temporary/transient states from concurrent installs
            if (dirname.ends_with(".installing")
                || dirname.ends_with(".willBeDelete")
                || dirname.starts_with(".tmp_install_"))
            {
                continue;
            }

            // Layer 2: list subdirectory entries
            std::string subdir_path = dirname + "/";
            auto subdir_entries = provider.ListDirectory(subdir_path, false);

            // Check deletion marker
            if (HasFileEntry(subdir_entries, dirname + ".willBeDelete"))
            {
                continue;
            }

            // Find manifest
            auto manifest_name = FindManifestInEntries(subdir_entries, dirname);
            if (manifest_name.empty())
            {
                continue;
            }

            // Read manifest content
            std::string relative_manifest = subdir_path + manifest_name;
            auto        content = provider.ReadFile(relative_manifest);
            if (content.empty())
            {
                continue;
            }

            // Parse JSON
            try
            {
                auto parsed = Das::Utils::ParseYyjsonFromString(
                    content,
                    yyjson::ReadFlag::AllowComments
                        | yyjson::ReadFlag::AllowTrailingCommas);
                if (!parsed)
                {
                    continue;
                }
                const auto& const_val = *parsed;
                auto        obj = const_val.as_object();
                if (!obj)
                {
                    continue;
                }
                PluginPackageDesc desc;
                ParsePluginPackageDescFromJson(*obj, desc);

                result.push_back(std::move(desc));
            }
            catch (const std::exception&)
            {
                // Parse failure — skip
            }
        }
        else
        {
            // ── Flat-file mode: .json manifest at root ──
            auto path = entry.name;
            if (path.size() < 6 || path.substr(path.size() - 5) != ".json")
            {
                continue;
            }

            auto stem = path.substr(0, path.size() - 5);

            // Check flat-file deletion marker
            if (HasFileEntry(root_entries, stem + ".willBeDelete"))
            {
                continue;
            }

            // Read and parse
            auto content = provider.ReadFile(entry.name);
            if (content.empty())
            {
                continue;
            }

            try
            {
                auto parsed = Das::Utils::ParseYyjsonFromString(
                    content,
                    yyjson::ReadFlag::AllowComments
                        | yyjson::ReadFlag::AllowTrailingCommas);
                if (!parsed)
                {
                    continue;
                }
                const auto& const_val = *parsed;
                auto        obj = const_val.as_object();
                if (!obj)
                {
                    continue;
                }
                PluginPackageDesc desc;
                ParsePluginPackageDescFromJson(*obj, desc);

                result.push_back(std::move(desc));
            }
            catch (const std::exception&)
            {
                // Not a valid manifest — skip
            }
        }
    }

    return result;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINSCANNER_H
