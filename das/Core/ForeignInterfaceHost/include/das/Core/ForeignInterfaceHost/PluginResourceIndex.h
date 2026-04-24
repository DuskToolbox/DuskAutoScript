#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINRESOURCEINDEX_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINRESOURCEINDEX_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>

#include <filesystem>
#include <shared_mutex>
#include <unordered_map>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

/**
 * @brief A single entry in the GUID -> resource mapping cache.
 *
 * Each entry captures the full metadata needed to resolve a plugin's
 * resource root from a lookup GUID. The entry is populated during
 * full-scan rebuild and published atomically.
 */
struct PluginResourceEntry
{
    DasGuid               lookup_guid;
    DasGuid               plugin_guid;
    std::filesystem::path manifest_path;
    std::filesystem::path package_root;
    std::filesystem::path resource_root;
    std::string           plugin_name;
};

/**
 * @brief DasCore-internal singleton providing GUID-based resource root lookup.
 *
 * Thread-safety model:
 *   - Read path (ResolvePluginResourceEntryByGuid): shared_lock
 *   - Scan rebuild: lock-free temp map construction, then short unique_lock
 *     to swap into the published map
 *   - No long-running operations under any lock
 */
class PluginResourceIndex
{
public:
    /**
     * @brief Get the process-wide singleton instance.
     *
     * The singleton is lazily constructed on first access. It starts with
     * no scan root configured and an empty cache.
     */
    static PluginResourceIndex& GetInstance();

    /**
     * @brief Configure the scan root directory (typically plugin_dir).
     *
     * This does NOT trigger eager scanning. The scan root is stored for
     * use during miss-triggered full scans.
     *
     * @param scan_root The root directory to scan for plugins.
     */
    void ConfigurePluginResourceScanRoot(std::filesystem::path scan_root);

    /**
     * @brief Resolve a resource entry by lookup GUID.
     *
     * If the GUID is not in the cache and a scan root has been configured,
     * a full scan is triggered before the retry lookup.
     *
     * @param lookup_guid The GUID to look up (typically from
     * IDasTypeInfo::GetGuid()).
     * @param pp_out      [out] Receives a pointer to the cached entry on
     * success. The pointer remains valid until the next scan rebuild.
     * @return DAS_S_OK on success, DAS_E_NOT_FOUND if the GUID is not present
     *         after scanning, or an error code on scan failure.
     */
    DasResult ResolvePluginResourceEntryByGuid(
        const DasGuid&              lookup_guid,
        const PluginResourceEntry** pp_out);

    /**
     * @brief Force a full rescan on the next cache miss.
     *
     * Marks the cache as stale so the next ResolvePluginResourceEntryByGuid
     * call that misses will trigger a full rebuild. Primarily for testing.
     */
    void InvalidateCache();

private:
    PluginResourceIndex() = default;

    /**
     * @brief Perform a full scan of the configured scan root.
     *
     * Enumerates directory entries under scan_root_, parses manifests,
     * validates resourcePath, and builds a temporary guid->entry map.
     * On success, atomically replaces the published map.
     *
     * @return DAS_S_OK on success, or an error code on failure.
     */
    DasResult ScanAndPublish();

    /**
     * @brief Validate that resourcePath is safe and compute resource_root.
     *
     * @param package_root  The plugin package root directory.
     * @param resource_path The resourcePath value from the manifest.
     * @param resource_root [out] The computed resource root on success.
     * @return DAS_S_OK if valid, DAS_E_INVALID_PATH otherwise.
     */
    static DasResult ValidateResourcePath(
        const std::filesystem::path& package_root,
        const std::string&           resource_path,
        std::filesystem::path&       resource_root);

    std::shared_mutex     mutex_;
    std::filesystem::path scan_root_;
    bool                  scan_root_configured_ = false;
    bool                  cache_stale_ = true;
    std::unordered_map<DasGuid, PluginResourceEntry> entries_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINRESOURCEINDEX_H
