#ifndef DAS_CORE_SETTINGSMANAGER_GLOBALSETTINGSMANAGER_H
#define DAS_CORE_SETTINGSMANAGER_GLOBALSETTINGSMANAGER_H

#include <das/Core/SettingsManager/Config.h>
#include <das/ExportInterface/DasJson.h>
#include <das/ExportInterface/IDasSettings.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <mutex>
#include <nlohmann/json.hpp>

NLOHMANN_JSON_SERIALIZE_ENUM(
    DasType,
    {{DAS_TYPE_INT, "int"},
     {DAS_TYPE_FLOAT, "float"},
     {DAS_TYPE_STRING, "string"},
     {DAS_TYPE_BOOL, "bool"}})

DAS_CORE_SETTINGSMANAGER_NS_BEGIN

class DasSettings;

class IDasSettingsForUiImpl final : public IDasSettingsForUi
{
    DasSettings& impl_;

public:
    IDasSettingsForUiImpl(DasSettings& impl);
    // IDasBase
    int64_t  AddRef() override;
    int64_t  Release() override;
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_object) override;
    // IDasSettingsForUi
    DAS_IMPL ToString(IDasReadOnlyString** pp_out_string) override;
    DAS_IMPL FromString(IDasReadOnlyString* p_in_settings) override;
    DAS_IMPL SaveToWorkingDirectory(
        IDasReadOnlyString* p_relative_path) override;
    DAS_IMPL Save() override;
};

/**
 * @brief 全局单例，不需要释放
 */
class DasSettings
{
    std::mutex     mutex_;
    nlohmann::json settings_;
    /**
     * @brief 默认设置就是和设置一样的结构，只是里面存了默认值
     *
     */
    nlohmann::json        default_values_;
    std::filesystem::path path_;

    IDasSettingsForUiImpl cpp_projection_for_ui_{*this};

    auto GetKey(const char* p_type_name, const char* key)
        -> Utils::Expected<std::reference_wrapper<const nlohmann::json>>;

    auto FindTypeSettings(const char* p_type_name)
        -> Utils::Expected<std::reference_wrapper<const nlohmann::json>>;

    auto SaveImpl(const std::filesystem::path& full_path) -> DasResult;

public:
    // IDasBase
    int64_t AddRef();
    int64_t Release();

    // IDasSettingsForUi
    DasResult ToString(IDasReadOnlyString** pp_out_string);
    DasResult FromString(IDasReadOnlyString* p_in_settings);
    DasResult SaveToWorkingDirectory(IDasReadOnlyString* p_relative_path);
    DasResult Save();
    // DasSettings
    /**
     * @brief Set the Default Values object
     *
     * @param rv_json rvalue of json. You should move it to this function.
     * @return DasResult
     */
    DasResult SetDefaultValues(nlohmann::json&& rv_json);
    DasResult LoadSettings(IDasReadOnlyString* p_path);
    // to projection
    operator IDasSettingsForUiImpl*() noexcept;
};

extern DasPtr<DasSettings> g_settings;

DAS_CORE_SETTINGSMANAGER_NS_END

#endif // DAS_CORE_SETTINGSMANAGER_GLOBALSETTINGSMANAGER_H
