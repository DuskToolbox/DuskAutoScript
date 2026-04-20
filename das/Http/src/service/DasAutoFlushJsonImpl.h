#pragma once

#include <das/IDasSettingsService.h>
#include <das/_autogen/idl/abi/DasJson.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasJson.Implements.hpp>
#include <string>
#include <unordered_set>

namespace Das::Http
{

    /**
     * @brief IDasJson 实现：通过 SettingsManager 读写，每次 Set 自动落盘。
     * 持有 SettingsManager& 裸引用，调用方需保证 SettingsManager 存活。
     * 白名单外的字段访问返回 DAS_E_PERMISSION_DENIED。
     * 支持 path_prefix_ 实现嵌套 JSON 子对象访问。
     */
    class DasAutoFlushJsonImpl final
        : public Das::ExportInterface::DasJsonImplBase<DasAutoFlushJsonImpl>
    {
    public:
        DasAutoFlushJsonImpl(
            IDasSettingsService&            settings_service,
            std::string                     profile_id,
            std::string                     plugin_guid,
            std::unordered_set<std::string> whitelist,
            std::string                     path_prefix = {});

        DAS_IMPL GetIntByName(IDasReadOnlyString* key, int64_t* p_out_int)
            override;
        DAS_IMPL GetFloatByName(IDasReadOnlyString* key, float* p_out_float)
            override;
        DAS_IMPL GetStringByName(
            IDasReadOnlyString*  key,
            IDasReadOnlyString** pp_out_string) override;
        DAS_IMPL GetBoolByName(IDasReadOnlyString* key, bool* p_out_bool)
            override;
        DAS_IMPL GetObjectRefByName(
            IDasReadOnlyString* key,
            IDasJson**          pp_out_das_json) override;

        DAS_IMPL SetIntByName(IDasReadOnlyString* key, int64_t in_int) override;
        DAS_IMPL SetFloatByName(IDasReadOnlyString* key, float in_float)
            override;
        DAS_IMPL SetStringByName(
            IDasReadOnlyString* key,
            IDasReadOnlyString* p_in_string) override;
        DAS_IMPL SetBoolByName(IDasReadOnlyString* key, bool in_bool) override;
        DAS_IMPL SetObjectByName(
            IDasReadOnlyString* key,
            IDasJson*           p_in_das_json) override;

        DAS_IMPL GetIntByIndex(size_t index, int64_t* p_out_int) override;
        DAS_IMPL GetFloatByIndex(size_t index, float* p_out_float) override;
        DAS_IMPL GetStringByIndex(
            size_t               index,
            IDasReadOnlyString** pp_out_string) override;
        DAS_IMPL GetBoolByIndex(size_t index, bool* p_out_bool) override;
        DAS_IMPL GetObjectRefByIndex(size_t index, IDasJson** pp_out_das_json)
            override;
        DAS_IMPL SetIntByIndex(size_t index, int64_t in_int) override;
        DAS_IMPL SetFloatByIndex(size_t index, float in_float) override;
        DAS_IMPL SetStringByIndex(size_t index, IDasReadOnlyString* p_in_string)
            override;
        DAS_IMPL SetBoolByIndex(size_t index, bool in_bool) override;
        DAS_IMPL SetObjectByIndex(size_t index, IDasJson* p_in_das_json)
            override;

        DAS_IMPL GetTypeByName(
            IDasReadOnlyString*            key,
            Das::ExportInterface::DasType* p_out_type) override;
        DAS_IMPL GetTypeByIndex(
            size_t                         index,
            Das::ExportInterface::DasType* p_out_type) override;
        DAS_IMPL GetSize(uint64_t* p_out_size) override;
        DAS_IMPL ToString(int32_t indent, IDasReadOnlyString** pp_out_string)
            override;
        DAS_IMPL Clear() override;

    private:
        IDasSettingsService&            settings_service_;
        std::string                     profile_id_;
        std::string                     plugin_guid_;
        std::unordered_set<std::string> whitelist_;
        std::string                     path_prefix_;

        DasResult CheckWhitelist(IDasReadOnlyString* key, std::string& out_key);

        std::string MakeFullPath(const std::string& key) const;
        bool        IsPrefixAllowed(const std::string& prefix) const;

        nlohmann::json GetField(const std::string& full_path);
        DasResult      SetField(
            const std::string&    full_path,
            const nlohmann::json& value);

        nlohmann::json GetCurrentJson();
    };

} // namespace Das::Http
