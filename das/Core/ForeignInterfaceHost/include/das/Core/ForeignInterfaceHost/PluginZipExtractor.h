#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINZIPEXTRACTOR_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINZIPEXTRACTOR_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/DasExport.h>
#include <das/IDasBase.h>

#include <filesystem>
#include <string>
#include <string_view>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

/**
 * @brief Extract and install a plugin package (in-memory ZIP) atomically.
 *
 * Extracts into a temp directory under plugin_dir and renames into place.
 * Entry names are validated against a whole-archive filename whitelist
 * (no "..", no ':', no trailing dot/space, no reserved device names, no
 * absolute or mixed-separator traversal) before any disk write, plus a
 * per-entry containment check.
 *
 * Flat packages (no subdirectories) are auto-wrapped into a directory:
 * the manifest candidate is root-level manifest.json first, then the
 * remaining root-level .json entries in entry order (a candidate must
 * parse to a JSON object with a "name" or "guid" string field). For a
 * manifest.json candidate the directory name is the manifest's "name"
 * field; for any other X.json candidate the directory name is the file
 * stem X, so the installed layout X/X.json matches the scanner's primary
 * <dirname>.json discovery convention.
 *
 * @param plugin_dir Destination plugin directory.
 * @param zip_data In-memory plugin package bytes.
 * @return DAS_S_OK on success; DAS_E_INVALID_ARGUMENT for malformed or
 *         rejected packages; DAS_E_FAIL on filesystem errors.
 */
DAS_EXPORT DasResult InstallPlugin(
    const std::filesystem::path& plugin_dir,
    std::string_view             zip_data);

/**
 * @brief Read plugin manifest metadata from an in-memory plugin package.
 *
 * Enumerates the ZIP central directory and reads only the manifest JSON entry
 * from memory. Does not extract files and does not mutate the filesystem.
 * Used by HTTP/plugin package config domain before acquiring per-plugin
 * inflight keys.
 *
 * @param zip_data In-memory plugin package bytes.
 * @param out_guid Parsed manifest guid field (string representation).
 * @param out_name Parsed manifest name field.
 * @return DAS_S_OK on success; DAS_E_INVALID_ARGUMENT if no manifest
 *         candidate is found, or none of them is readable (including too
 *         large) and parses to a JSON object containing a guid/name string
 *         field.
 *
 * @architecture HTTP config domain. Called from
 * PluginManagerServiceImpl::InstallPluginPackageData to extract plugin
 * identity before any filesystem mutation. Container parsing, CRC32
 * integrity, zip64 handling, and entry-name encoding normalization are
 * delegated to libzip; this module keeps the path-traversal defense and
 * the decompression/entry-size caps, and reads the manifest purely from
 * memory.
 */
DAS_EXPORT DasResult ReadPluginManifestMetadataFromZip(
    std::string_view zip_data,
    std::string&     out_guid,
    std::string&     out_name);

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINZIPEXTRACTOR_H
