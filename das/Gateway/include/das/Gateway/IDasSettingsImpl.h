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

// {A59BFE7D-1A4D-4988-8A18-8A3D86CC2C9E}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Gateway,
    DasSettings,
    0xa59bfe7d,
    0x1a4d,
    0x4988,
    0x8a,
    0x18,
    0x8a,
    0x3d,
    0x86,
    0xcc,
    0x2c,
    0x9e);

DAS_GATEWAY_NS_BEGIN

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

/**
 * @brief Core
 * 里面会直接使用DasSettings，但是直接导出类会报warning，所以这里只导出必要接口
 */
class DasSettings
{
    Utils::RefCounter<DasSettings>          ref_counter_;
    std::mutex                              mutex_;
    DasPtr<IDasJson>                        settings_;
    std::filesystem::path                   path_;
    DasPtr<IDasJsonSettingOnDeletedHandler> p_handler_;

    IDasJsonSettingImpl cpp_projection_for_ui_{*this};

    auto SaveImpl(const std::filesystem::path& full_path) -> DasResult;

public:
    // IDasBase
    DAS_GATEWAY_API int64_t AddRef();
    DAS_GATEWAY_API int64_t Release();

    // IDasJsonSetting
    DAS_GATEWAY_API DasResult ToString(IDasReadOnlyString** pp_out_string);
    DAS_GATEWAY_API DasResult FromString(IDasReadOnlyString* p_in_settings);
    DAS_GATEWAY_API DasResult
    SaveToWorkingDirectory(IDasReadOnlyString* p_relative_path);
    DAS_GATEWAY_API DasResult Save();
    DAS_GATEWAY_API DasResult
    SetOnDeletedHandler(IDasJsonSettingOnDeletedHandler* p_handler);
    // DasSettings
    DasResult LoadSettings(IDasReadOnlyString* p_path);
    DasResult InitSettings(
        IDasReadOnlyString* p_path,
        IDasReadOnlyString* p_json_string);
    DasResult            OnDeleted();
    DAS_GATEWAY_API void SetJson(IDasJson* p_json);
    DAS_GATEWAY_API void GetJson(IDasJson** pp_out_json);
    // to projection
    operator IDasJsonSettingImpl*() noexcept;

    void Delete();
};

extern DasPtr<DasSettings> g_settings;

DAS_GATEWAY_NS_END

#endif // DAS_GATEWAY_IDASSETTINSIMPL_H
