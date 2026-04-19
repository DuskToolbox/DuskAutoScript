#pragma once

#include <das/Core/SettingsManager/SettingsManager.h>
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
     */
    class DasAutoFlushJsonImpl final
        : public Das::ExportInterface::DasJsonImplBase<DasAutoFlushJsonImpl>
    {
    public:
        DasAutoFlushJsonImpl(
            Das::Core::SettingsManager::SettingsManager& settings_manager,
            std::string                                  profile_id,
            std::string                                  plugin_guid,
            std::unordered_set<std::string>              whitelist);

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

        /** ByIndex 方法不适用（settings 是 JSON object 而非 array），均返回
         * DAS_E_NOT_FOUND */
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
        DAS_IMPL ToString(int32_t indent, IDasReadOnlyString** pp_out_string)
            override;
        DAS_IMPL Clear() override;

    private:
        Das::Core::SettingsManager::SettingsManager& settings_manager_;
        std::string                                  profile_id_;
        std::string                                  plugin_guid_;
        std::unordered_set<std::string>              whitelist_;

        /**
         * @brief 提取 UTF-8 key 并检查白名单
         * @param key 输入的只读字符串 key
         * @param[out] out_key 提取后的 UTF-8 字符串
         * @return DAS_S_OK 如果 key 在白名单中，DAS_E_PERMISSION_DENIED
         * 如果不在
         */
        DasResult CheckWhitelist(IDasReadOnlyString* key, std::string& out_key);
    };

} // namespace Das::Http
