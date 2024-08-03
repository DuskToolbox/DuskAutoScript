#ifndef ASR_CORE_SETTINGSMANAGER_GLOBALSETTINGSMANAGER_H
#define ASR_CORE_SETTINGSMANAGER_GLOBALSETTINGSMANAGER_H

#include <AutoStarRail/Core/SettingsManager/Config.h>
#include <AutoStarRail/ExportInterface/AsrJson.h>
#include <AutoStarRail/ExportInterface/IAsrSettings.h>
#include <AutoStarRail/Utils/CommonUtils.hpp>
#include <AutoStarRail/Utils/Expected.h>
#include <mutex>
#include <nlohmann/json.hpp>

NLOHMANN_JSON_SERIALIZE_ENUM(
    AsrType,
    {{ASR_TYPE_INT, "int"},
     {ASR_TYPE_FLOAT, "float"},
     {ASR_TYPE_STRING, "string"},
     {ASR_TYPE_BOOL, "bool"}})

ASR_CORE_SETTINGSMANAGER_NS_BEGIN

class AsrSettings;

class IAsrSettingsForUiImpl final : public IAsrSettingsForUi
{
    AsrSettings& impl_;

public:
    IAsrSettingsForUiImpl(AsrSettings& impl);
    // IAsrBase
    int64_t  AddRef() override;
    int64_t  Release() override;
    ASR_IMPL QueryInterface(const AsrGuid& iid, void** pp_object) override;
    // IAsrSettingsForUi
    ASR_IMPL ToString(IAsrReadOnlyString** pp_out_string) override;
    ASR_IMPL FromString(IAsrReadOnlyString* p_in_settings) override;
    ASR_IMPL SaveToWorkingDirectory(
        IAsrReadOnlyString* p_relative_path) override;
    ASR_IMPL Save() override;
};

/**
 * @brief 全局单例，不需要释放
 */
class AsrSettings
{
    std::mutex     mutex_;
    nlohmann::json settings_;
    /**
     * @brief 默认设置就是和设置一样的结构，只是里面存了默认值
     *
     */
    nlohmann::json        default_values_;
    std::filesystem::path path_;

    IAsrSettingsForUiImpl cpp_projection_for_ui_{*this};

    auto GetKey(const char* p_type_name, const char* key)
        -> Utils::Expected<std::reference_wrapper<const nlohmann::json>>;

    auto FindTypeSettings(const char* p_type_name)
        -> Utils::Expected<std::reference_wrapper<const nlohmann::json>>;

    auto SaveImpl(const std::filesystem::path& full_path) -> AsrResult;

public:
    // IAsrBase
    int64_t AddRef();
    int64_t Release();

    // IAsrSettingsForUi
    AsrResult ToString(IAsrReadOnlyString** pp_out_string);
    AsrResult FromString(IAsrReadOnlyString* p_in_settings);
    AsrResult SaveToWorkingDirectory(IAsrReadOnlyString* p_relative_path);
    AsrResult Save();
    // AsrSettings
    /**
     * @brief Set the Default Values object
     *
     * @param rv_json rvalue of json. You should move it to this function.
     * @return AsrResult
     */
    AsrResult SetDefaultValues(nlohmann::json&& rv_json);
    AsrResult LoadSettings(IAsrReadOnlyString* p_path);
    // to projection
    operator IAsrSettingsForUiImpl*() noexcept;
};

extern AsrPtr<AsrSettings> g_settings;

ASR_CORE_SETTINGSMANAGER_NS_END

#endif // ASR_CORE_SETTINGSMANAGER_GLOBALSETTINGSMANAGER_H
