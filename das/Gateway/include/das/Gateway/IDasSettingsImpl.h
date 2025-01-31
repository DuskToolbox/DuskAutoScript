#ifndef DAS_GATEWAY_IDASSETTINSIMPL_H
#define DAS_GATEWAY_IDASSETTINSIMPL_H

#include <das/ExportInterface/DasJson.h>
#include <das/ExportInterface/IDasSettings.h>
#include <das/Gateway/Config.h>
#include <das/Gateway/IDasSettingsImpl.h>
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

DAS_GATEWAY_NS_BEGIN

class DasSettings;

class IDasJsonSettingImpl final : public IDasJsonSetting
{
    DasSettings& impl_;

public:
    IDasJsonSettingImpl(DasSettings& impl);
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
    DAS_IMPL SetOnDeletedHandler(
        IDasJsonSettingOnDeletedHandler* p_handler) override;
};

class DasSettings
{
    Utils::RefCounter<DasSettings>          ref_counter_;
    std::mutex                              mutex_;
    nlohmann::json                          settings_;
    std::filesystem::path                   path_;
    DasPtr<IDasJsonSettingOnDeletedHandler> p_handler_;

    IDasJsonSettingImpl cpp_projection_for_ui_{*this};

    auto GetKey(const char* p_type_name, const char* key)
        -> Utils::Expected<std::reference_wrapper<const nlohmann::json>>;

    auto FindTypeSettings(const char* p_type_name)
        -> Utils::Expected<std::reference_wrapper<const nlohmann::json>>;

    auto SaveImpl(const std::filesystem::path& full_path) -> DasResult;

public:
    // IDasBase
    int64_t AddRef();
    int64_t Release();

    // IDasJsonSetting
    DasResult ToString(IDasReadOnlyString** pp_out_string);
    DasResult FromString(IDasReadOnlyString* p_in_settings);
    DasResult SaveToWorkingDirectory(IDasReadOnlyString* p_relative_path);
    DasResult Save();
    DasResult SetOnDeletedHandler(IDasJsonSettingOnDeletedHandler* p_handler);
    // DasSettings
    DasResult LoadSettings(IDasReadOnlyString* p_path);
    DasResult InitSettings(
        IDasReadOnlyString* p_path,
        IDasReadOnlyString* p_json_string);
    // to projection
    operator IDasJsonSettingImpl*() noexcept;

    void Delete();
};

extern DasPtr<DasSettings> g_settings;

DAS_GATEWAY_NS_END

#endif // DAS_GATEWAY_IDASSETTINSIMPL_H
