#ifndef DAS_CORE_SETTINGS_MANAGER_SETTINGS_MANAGER_H
#define DAS_CORE_SETTINGS_MANAGER_SETTINGS_MANAGER_H

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/SettingsManager/Config.h>
#include <das/DasExport.h>
#include <das/DasTypes.hpp>
#include <das/IDasSettingsService.h>
#include <das/Utils/DasJsonCore.h>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <unordered_map>

DAS_CORE_SETTINGS_MANAGER_NS_BEGIN

/// Per-key state cell: one shared_mutex + one snapshot per settings file key.
/// Keys follow the pattern: "global/ui", "profile/{pid}/ui",
/// "profile/{pid}/plugin/{guid}", "profile/{pid}/scheduler",
/// "profile/{pid}/task/{taskId}".
///
/// Thread model (Phase 52 per-key domain):
///   - cells_mutex_ protects ONLY map lookup/create for these cells.
///   - Each cell's mutex covers the full RMW cycle for that key:
///     read current value -> apply mutation -> WriteJsonFile -> update
///     snapshot.
///   - Different keys use independent shared_mutex instances and do not block
///     each other.
///   - Read operations use shared_lock (concurrent readers).
///   - Write/RMW operations use unique_lock (exclusive access).
struct SettingsKeyCell
{
    std::shared_mutex             mutex;
    yyjson::writer::detail::value snapshot;

    SettingsKeyCell() = default;
    SettingsKeyCell(SettingsKeyCell&& other) noexcept
        : snapshot(std::move(other.snapshot))
    {
    }
    SettingsKeyCell& operator=(SettingsKeyCell&& other) noexcept
    {
        if (this != &other)
        {
            snapshot = std::move(other.snapshot);
        }
        return *this;
    }
    // Non-copyable (shared_mutex is not copyable)
    SettingsKeyCell(const SettingsKeyCell&) = delete;
    SettingsKeyCell& operator=(const SettingsKeyCell&) = delete;
};

class SettingsManager
{
public:
    explicit SettingsManager(const std::filesystem::path& base_dir);
    ~SettingsManager() = default;

    // Global Settings (settings/ui.json)
    std::string                   GetGlobalSettings();
    yyjson::writer::detail::value GetGlobalSettingsJson();
    DasResult UpdateGlobalSettings(const std::string& json_str);
    DasResult UpdateGlobalSettingsJson(
        const yyjson::writer::detail::value& data);

    // Profile management (settings/${pid}/)
    std::string                   GetProfileList();
    yyjson::writer::detail::value GetProfileListJson();
    DasResult                     CreateProfile(const std::string& profile_id);
    DasResult                     DeleteProfile(const std::string& profile_id);

    // Profile data (settings/${pid}/ui.json)
    std::string                   GetProfile(const std::string& profile_id);
    yyjson::writer::detail::value GetProfileJson(const std::string& profile_id);
    DasResult                     UpdateProfile(
        const std::string& profile_id,
        const std::string& json_str);
    DasResult UpdateProfileJson(
        const std::string&                   profile_id,
        const yyjson::writer::detail::value& data);

    // Plugin settings (settings/${pid}/${pluginGuid}.json)
    yyjson::writer::detail::value GetPluginSettingsJson(
        const std::string& profile_id,
        const std::string& guid);

    /// Get plugin settings with status. Returns {json, DAS_S_OK} for valid
    /// files, {rebuilt_empty_json, DAS_S_FALSE} when the file was corrupt or
    /// missing and rebuilt from defaults.
    std::pair<yyjson::writer::detail::value, DasResult>
                GetPluginSettingsWithStatus(
                    const std::string& profile_id,
                    const std::string& guid);
    std::string GetPluginSettings(
        const std::string& profile_id,
        const std::string& guid);
    DasResult UpdatePluginSettings(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& json_str);
    DasResult UpdatePluginSettingsJson(
        const std::string&                   profile_id,
        const std::string&                   guid,
        const yyjson::writer::detail::value& data);

    // Plugin settings field-level access (JSON object, no serialization)
    yyjson::writer::detail::value GetPluginSettingsFieldJson(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& field_name);
    DasResult UpdatePluginSettingsFieldJson(
        const std::string&                   profile_id,
        const std::string&                   guid,
        const std::string&                   field_name,
        const yyjson::writer::detail::value& value);

    // Plugin settings field-level access (string-based, legacy)
    std::string GetPluginSettingsField(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& field_name);
    DasResult UpdatePluginSettingsField(
        const std::string& profile_id,
        const std::string& guid,
        const std::string& field_name,
        const std::string& field_json_value);

    // Plugin settings recovery: rebuild from manifest defaults if file is
    // corrupt or missing. Returns DAS_S_FALSE when defaults were restored.
    DasResult RebuildPluginSettingsFromDefaults(
        const std::string&              profile_id,
        const std::string&              guid,
        const std::vector<std::string>& field_names,
        const std::vector<std::string>& default_values);

    // Scheduler state (settings/${pid}/scheduler.json)
    yyjson::writer::detail::value GetSchedulerIndexJson(
        const std::string& profile_id);
    DasResult UpdateSchedulerIndexJson(
        const std::string&                   profile_id,
        const yyjson::writer::detail::value& scheduler_json);

    // Task instance (settings/${pid}/taskId${taskId}.json)
    yyjson::writer::detail::value GetTaskInstanceJson(
        const std::string& profile_id,
        int64_t            task_id);
    DasResult UpdateTaskInstanceJson(
        const std::string&                   profile_id,
        int64_t                              task_id,
        const yyjson::writer::detail::value& task_json);
    DasResult DeleteTaskInstanceJson(
        const std::string& profile_id,
        int64_t            task_id);

    /// Register a callback to be invoked when settings change.
    void SetSettingsNotifyCallback(SettingsNotifyFunc func, void* user_data);

private:
    std::filesystem::path GetProfileDir(const std::string& profile_id) const;
    std::filesystem::path GetProfileUiPath(const std::string& profile_id) const;
    std::filesystem::path GetPluginSettingsPath(
        const std::string& profile_id,
        const std::string& guid) const;
    std::filesystem::path GetSchedulerIndexPath(
        const std::string& profile_id) const;
    std::filesystem::path GetTaskInstancePath(
        const std::string& profile_id,
        int64_t            task_id) const;

    // File I/O helpers
    static std::string ReadJsonFile(const std::filesystem::path& path);
    static DasResult   WriteJsonFile(
        const std::filesystem::path& path,
        const std::string&           json_str);
    static DasResult WriteJsonFile(
        const std::filesystem::path&         path,
        const yyjson::writer::detail::value& data);

    /// Find or create per-key state cell. Uses cells_mutex_ for short
    /// lookup/create with double-checked locking pattern.
    /// Returns non-null pointer to the cell. The cell's mutex is NOT locked
    /// by this call.
    SettingsKeyCell* GetOrCreateCell(const std::string& key);

    std::filesystem::path base_dir_;

    /// Per-key state cells: one shared_mutex + snapshot per settings file key.
    /// Access patterns:
    ///   - cells_mutex_ protects ONLY map lookup/insert (no I/O under this
    ///     lock).
    ///   - Each cell's mutex covers the full RMW cycle for that specific key.
    std::unordered_map<std::string, SettingsKeyCell> key_cells_;

    /// Registry/map lock: used ONLY for finding/creating per-key state cells.
    /// NEVER covers file I/O (WriteJsonFile/ReadJsonFile/exists/directory_ite
    /// rator).
    mutable std::shared_mutex cells_mutex_;

    struct SettingsNotify
    {
        SettingsNotifyFunc func = nullptr;
        void*              user_data = nullptr;

        void     operator()(const char* json) const { func(json, user_data); }
        explicit operator bool() const { return func != nullptr; }
    } settings_notify_;
};

DAS_CORE_SETTINGS_MANAGER_NS_END

#endif // DAS_CORE_SETTINGS_MANAGER_SETTINGS_MANAGER_H
