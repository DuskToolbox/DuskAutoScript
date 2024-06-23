#ifndef ASR_CORE_SETTINGSMANAGER_GLOBALSETTINGSMANAGER_H
#define ASR_CORE_SETTINGSMANAGER_GLOBALSETTINGSMANAGER_H

#include <AutoStarRail/Core/SettingsManager/Config.h>
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

class IAsrSettingsImpl final : public IAsrSettings
{
    AsrSettings& impl_;
    std::string  u8_type_name_{};

public:
    IAsrSettingsImpl(AsrSettings& impl, const char* u8_type_name);
    // IAsrBase
    ASR_UTILS_IASRBASE_AUTO_IMPL(IAsrSettingsImpl)
    ASR_IMPL QueryInterface(const AsrGuid& iid, void** pp_object) override;
    // IAsrSettings
    ASR_IMPL GetString(
        IAsrReadOnlyString*  key,
        IAsrReadOnlyString** pp_out_string) override;
    ASR_IMPL GetBool(IAsrReadOnlyString* key, bool* p_out_bool) override;
    ASR_IMPL GetInt(IAsrReadOnlyString* key, int64_t* p_out_int) override;
    ASR_IMPL GetFloat(IAsrReadOnlyString* key, float* p_out_float) override;

    ASR_METHOD SetString(IAsrReadOnlyString* key, IAsrReadOnlyString* value)
        override;
    ASR_METHOD SetBool(IAsrReadOnlyString* key, bool value) override;
    ASR_METHOD SetInt(IAsrReadOnlyString* key, int64_t value) override;
    ASR_METHOD SetFloat(IAsrReadOnlyString* key, float value) override;
};

class IAsrSwigSettingsImpl final : public IAsrSwigSettings
{
    AsrSettings& impl_;
    std::string  u8_type_name_{};

public:
    IAsrSwigSettingsImpl(AsrSettings& impl, const char* u8_type_name);
    // IAsrSwigBase
    ASR_UTILS_IASRBASE_AUTO_IMPL(IAsrSwigSettingsImpl)
    AsrRetSwigBase QueryInterface(const AsrGuid& iid) override;
    // IAsrSwigSettings
    AsrRetReadOnlyString GetString(AsrReadOnlyString key) override;
    AsrRetBool           GetBool(AsrReadOnlyString key) override;
    AsrRetInt            GetInt(AsrReadOnlyString key) override;
    AsrRetFloat          GetFloat(AsrReadOnlyString key) override;

    AsrResult SetString(AsrReadOnlyString key, AsrReadOnlyString value)
        override;
    AsrResult SetBool(AsrReadOnlyString key, bool value) override;
    AsrResult SetInt(AsrReadOnlyString key, int64_t value) override;
    AsrResult SetFloat(AsrReadOnlyString key, float value) override;
};

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
    ASR_IMPL SaveToWorkingDirectory(IAsrReadOnlyString* p_relative_path) override;
    ASR_IMPL Save() override;
};

/**
 * @brief 全局单例
 */
class AsrSettings
{
    std::mutex     mutex_;
    nlohmann::json settings_;
    /**
     * @brief 默认设置就是和设置一样的结构，只是里面存了默认值
     *
     */
    nlohmann::json default_values_;
    std::filesystem::path path_;

    IAsrSettingsForUiImpl cpp_projection_for_ui_{*this};

    auto GetKey(const char* p_type_name, const char* key)
        -> Utils::Expected<std::reference_wrapper<const nlohmann::json>>;

    auto FindTypeSettings(const char* p_type_name)
        -> Utils::Expected<std::reference_wrapper<const nlohmann::json>>;

    auto SaveImpl(const std::filesystem::path& full_path) -> AsrResult;

public:
    AsrSettings(std::filesystem::path path);
    ~AsrSettings() = default;

    int64_t AddRef();
    int64_t Release();

    // IAsrSettings
    AsrRetReadOnlyString GetString(
        std::string_view  u8_type_string,
        AsrReadOnlyString key);
    AsrRetBool  GetBool(std::string_view u8_type_string, AsrReadOnlyString key);
    AsrRetInt   GetInt(std::string_view u8_type_string, AsrReadOnlyString key);
    AsrRetFloat GetFloat(
        std::string_view  u8_type_string,
        AsrReadOnlyString key);
    AsrResult SetString(
        std::string_view  u8_type_string,
        AsrReadOnlyString key,
        AsrReadOnlyString value);
    AsrResult SetBool(
        std::string_view  u8_type_string,
        AsrReadOnlyString key,
        bool              value);
    AsrResult SetInt(
        std::string_view  u8_type_string,
        AsrReadOnlyString key,
        int64_t           value);
    AsrResult SetFloat(
        std::string_view  u8_type_string,
        AsrReadOnlyString key,
        float             value);
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

extern AsrSettings g_settings;

ASR_CORE_SETTINGSMANAGER_NS_END

#endif // ASR_CORE_SETTINGSMANAGER_GLOBALSETTINGSMANAGER_H
