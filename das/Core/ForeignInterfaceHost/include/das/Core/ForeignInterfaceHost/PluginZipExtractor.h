#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINZIPEXTRACTOR_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINZIPEXTRACTOR_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/DasExport.h>
#include <das/IDasBase.h>

#include <filesystem>
#include <string>
#include <string_view>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

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
 * @return DAS_S_OK on success; DAS_E_INVALID_ARGUMENT if the manifest is
 *         missing, malformed, unsafe, or lacks guid/name.
 *
 * @architecture HTTP config domain (Phase 52). Called from
 * PluginManagerServiceImpl::InstallPluginPackageData to extract plugin
 * identity before any filesystem mutation. Reuses existing ZIP parsing
 * infrastructure (EnumerateZipEntries, local header offset resolution,
 * stored/deflate decompression) — no new ZIP library introduced.
 */
DAS_EXPORT DasResult ReadPluginManifestMetadataFromZip(
    std::string_view zip_data,
    std::string&     out_guid,
    std::string&     out_name);

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINZIPEXTRACTOR_H
