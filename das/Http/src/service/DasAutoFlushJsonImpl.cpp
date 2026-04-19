#include "DasAutoFlushJsonImpl.h"

#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>
#include <nlohmann/json.hpp>

namespace Das::Http
{

    using Das::ExportInterface::DAS_TYPE_BOOL;
    using Das::ExportInterface::DAS_TYPE_FLOAT;
    using Das::ExportInterface::DAS_TYPE_INT;
    using Das::ExportInterface::DAS_TYPE_JSON_ARRAY;
    using Das::ExportInterface::DAS_TYPE_JSON_OBJECT;
    using Das::ExportInterface::DAS_TYPE_NULL;
    using Das::ExportInterface::DAS_TYPE_STRING;
    using Das::ExportInterface::DAS_TYPE_UINT;
    using Das::ExportInterface::DAS_TYPE_UNSUPPORTED;
    using Das::ExportInterface::DasType;

    namespace
    {

        DasType ToDasType(nlohmann::json::value_t type)
        {
            switch (type)
            {
            case nlohmann::json::value_t::null:
                return DAS_TYPE_NULL;
            case nlohmann::json::value_t::object:
                return DAS_TYPE_JSON_OBJECT;
            case nlohmann::json::value_t::array:
                return DAS_TYPE_JSON_ARRAY;
            case nlohmann::json::value_t::string:
                return DAS_TYPE_STRING;
            case nlohmann::json::value_t::boolean:
                return DAS_TYPE_BOOL;
            case nlohmann::json::value_t::number_integer:
                return DAS_TYPE_UINT;
            case nlohmann::json::value_t::number_unsigned:
                return DAS_TYPE_INT;
            case nlohmann::json::value_t::number_float:
                return DAS_TYPE_FLOAT;
            case nlohmann::json::value_t::binary:
                [[fallthrough]];
            case nlohmann::json::value_t::discarded:
                [[fallthrough]];
            default:
                return DAS_TYPE_UNSUPPORTED;
            }
        }

    } // namespace

    DasAutoFlushJsonImpl::DasAutoFlushJsonImpl(
        Das::Core::SettingsManager::SettingsManager& settings_manager,
        std::string                                  profile_id,
        std::string                                  plugin_guid,
        std::unordered_set<std::string>              whitelist)
        : settings_manager_{settings_manager},
          profile_id_{std::move(profile_id)},
          plugin_guid_{std::move(plugin_guid)}, whitelist_{std::move(whitelist)}
    {
    }

    DasResult DasAutoFlushJsonImpl::CheckWhitelist(
        IDasReadOnlyString* key,
        std::string&        out_key)
    {
        DAS_UTILS_CHECK_POINTER(key)

        const auto expected_u8_key = ToU8StringWithoutOwnership(key);
        if (!expected_u8_key)
        {
            return expected_u8_key.error();
        }
        out_key = expected_u8_key.value();

        if (whitelist_.find(out_key) == whitelist_.end())
        {
            DAS_CORE_LOG_ERROR(
                "Field '{}' is not in whitelist. Access denied.",
                out_key);
            return DAS_E_PERMISSION_DENIED;
        }

        return DAS_S_OK;
    }

    // === GetByName methods ===

    DasResult DasAutoFlushJsonImpl::GetIntByName(
        IDasReadOnlyString* key,
        int64_t*            p_out_int)
    {
        DAS_UTILS_CHECK_POINTER(p_out_int)

        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_str = settings_manager_.GetPluginSettingsField(
            profile_id_,
            plugin_guid_,
            u8_key);
        if (field_str.empty())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto json_val = nlohmann::json::parse(field_str);
            *p_out_int = json_val.get<int64_t>();
            return DAS_S_OK;
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_TYPE_ERROR;
        }
    }

    DasResult DasAutoFlushJsonImpl::GetFloatByName(
        IDasReadOnlyString* key,
        float*              p_out_float)
    {
        DAS_UTILS_CHECK_POINTER(p_out_float)

        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_str = settings_manager_.GetPluginSettingsField(
            profile_id_,
            plugin_guid_,
            u8_key);
        if (field_str.empty())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto json_val = nlohmann::json::parse(field_str);
            *p_out_float = json_val.get<float>();
            return DAS_S_OK;
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_TYPE_ERROR;
        }
    }

    DasResult DasAutoFlushJsonImpl::GetStringByName(
        IDasReadOnlyString*  key,
        IDasReadOnlyString** pp_out_string)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_string)

        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_str = settings_manager_.GetPluginSettingsField(
            profile_id_,
            plugin_guid_,
            u8_key);
        if (field_str.empty())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto json_val = nlohmann::json::parse(field_str);
            return CreateIDasReadOnlyStringFromUtf8(
                json_val.get_ref<const std::string&>().c_str(),
                pp_out_string);
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_TYPE_ERROR;
        }
    }

    DasResult DasAutoFlushJsonImpl::GetBoolByName(
        IDasReadOnlyString* key,
        bool*               p_out_bool)
    {
        DAS_UTILS_CHECK_POINTER(p_out_bool)

        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_str = settings_manager_.GetPluginSettingsField(
            profile_id_,
            plugin_guid_,
            u8_key);
        if (field_str.empty())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto json_val = nlohmann::json::parse(field_str);
            *p_out_bool = json_val.get<bool>();
            return DAS_S_OK;
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_TYPE_ERROR;
        }
    }

    DasResult DasAutoFlushJsonImpl::GetObjectRefByName(
        IDasReadOnlyString* key,
        IDasJson**          pp_out_das_json)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_das_json)

        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_str = settings_manager_.GetPluginSettingsField(
            profile_id_,
            plugin_guid_,
            u8_key);
        if (field_str.empty())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto json_val = nlohmann::json::parse(field_str);
            if (!json_val.is_object())
            {
                return DAS_E_TYPE_ERROR;
            }

            auto full_settings_str =
                settings_manager_.GetPluginSettings(profile_id_, plugin_guid_);
            auto  full_settings = nlohmann::json::parse(full_settings_str);
            auto& sub_object = full_settings[u8_key];

            // Create a new DasAutoFlushJsonImpl for the sub-object with same
            // whitelist
            auto* sub_impl = new DasAutoFlushJsonImpl(
                settings_manager_,
                profile_id_,
                plugin_guid_,
                whitelist_);
            sub_impl->AddRef();
            *pp_out_das_json = sub_impl;
            return DAS_S_OK;
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_TYPE_ERROR;
        }
        catch (const std::bad_alloc& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_OUT_OF_MEMORY;
        }
    }

    // === SetByName methods ===

    DasResult DasAutoFlushJsonImpl::SetIntByName(
        IDasReadOnlyString* key,
        int64_t             in_int)
    {
        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_json_str = std::to_string(in_int);
        return settings_manager_.UpdatePluginSettingsField(
            profile_id_,
            plugin_guid_,
            u8_key,
            field_json_str);
    }

    DasResult DasAutoFlushJsonImpl::SetFloatByName(
        IDasReadOnlyString* key,
        float               in_float)
    {
        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_json_str = std::to_string(in_float);
        return settings_manager_.UpdatePluginSettingsField(
            profile_id_,
            plugin_guid_,
            u8_key,
            field_json_str);
    }

    DasResult DasAutoFlushJsonImpl::SetStringByName(
        IDasReadOnlyString* key,
        IDasReadOnlyString* p_in_string)
    {
        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        DAS_UTILS_CHECK_POINTER(p_in_string)

        const auto expected_value = ToU8StringWithoutOwnership(p_in_string);
        if (!expected_value)
        {
            return expected_value.error();
        }
        const auto u8_value = expected_value.value();

        try
        {
            nlohmann::json field_json = u8_value;
            return settings_manager_.UpdatePluginSettingsField(
                profile_id_,
                plugin_guid_,
                u8_key,
                field_json.dump());
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_INVALID_JSON;
        }
    }

    DasResult DasAutoFlushJsonImpl::SetBoolByName(
        IDasReadOnlyString* key,
        bool                in_bool)
    {
        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_json_str = in_bool ? "true" : "false";
        return settings_manager_.UpdatePluginSettingsField(
            profile_id_,
            plugin_guid_,
            u8_key,
            field_json_str);
    }

    DasResult DasAutoFlushJsonImpl::SetObjectByName(
        IDasReadOnlyString* key,
        IDasJson*           p_in_das_json)
    {
        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        DAS_UTILS_CHECK_POINTER(p_in_das_json)

        // Get JSON string from the input IDasJson via ToString
        IDasReadOnlyString* p_json_str = nullptr;
        auto to_string_result = p_in_das_json->ToString(-1, &p_json_str);
        if (DAS::IsFailed(to_string_result))
        {
            return to_string_result;
        }

        const auto expected_value = ToU8StringWithoutOwnership(p_json_str);
        if (p_json_str)
        {
            p_json_str->Release();
        }
        if (!expected_value)
        {
            return expected_value.error();
        }

        return settings_manager_.UpdatePluginSettingsField(
            profile_id_,
            plugin_guid_,
            u8_key,
            expected_value.value());
    }

    // === ByIndex methods ===

    DasResult DasAutoFlushJsonImpl::GetIntByIndex(
        size_t   index,
        int64_t* p_out_int)
    {
        return DAS_E_NOT_FOUND;
    }

    DasResult DasAutoFlushJsonImpl::GetFloatByIndex(
        size_t index,
        float* p_out_float)
    {
        return DAS_E_NOT_FOUND;
    }

    DasResult DasAutoFlushJsonImpl::GetStringByIndex(
        size_t               index,
        IDasReadOnlyString** pp_out_string)
    {
        return DAS_E_NOT_FOUND;
    }

    DasResult DasAutoFlushJsonImpl::GetBoolByIndex(
        size_t index,
        bool*  p_out_bool)
    {
        return DAS_E_NOT_FOUND;
    }

    DasResult DasAutoFlushJsonImpl::GetObjectRefByIndex(
        size_t     index,
        IDasJson** pp_out_das_json)
    {
        return DAS_E_NOT_FOUND;
    }

    DasResult DasAutoFlushJsonImpl::SetIntByIndex(size_t index, int64_t in_int)
    {
        return DAS_E_NOT_FOUND;
    }

    DasResult DasAutoFlushJsonImpl::SetFloatByIndex(
        size_t index,
        float  in_float)
    {
        return DAS_E_NOT_FOUND;
    }

    DasResult DasAutoFlushJsonImpl::SetStringByIndex(
        size_t              index,
        IDasReadOnlyString* p_in_string)
    {
        return DAS_E_NOT_FOUND;
    }

    DasResult DasAutoFlushJsonImpl::SetBoolByIndex(size_t index, bool in_bool)
    {
        return DAS_E_NOT_FOUND;
    }

    DasResult DasAutoFlushJsonImpl::SetObjectByIndex(
        size_t    index,
        IDasJson* p_in_das_json)
    {
        return DAS_E_NOT_FOUND;
    }

    // === Type/query methods ===

    DasResult DasAutoFlushJsonImpl::GetTypeByName(
        IDasReadOnlyString* key,
        DasType*            p_out_type)
    {
        DAS_UTILS_CHECK_POINTER(p_out_type)

        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_str = settings_manager_.GetPluginSettingsField(
            profile_id_,
            plugin_guid_,
            u8_key);
        if (field_str.empty())
        {
            *p_out_type = DAS_TYPE_NULL;
            return DAS_S_OK;
        }

        try
        {
            auto json_val = nlohmann::json::parse(field_str);
            *p_out_type = ToDasType(json_val.type());
            return DAS_S_OK;
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_INVALID_JSON;
        }
    }

    DasResult DasAutoFlushJsonImpl::GetTypeByIndex(
        size_t   index,
        DasType* p_out_type)
    {
        return DAS_E_NOT_FOUND;
    }

    DasResult DasAutoFlushJsonImpl::ToString(
        int32_t              indent,
        IDasReadOnlyString** pp_out_string)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_string)

        try
        {
            auto full_settings_str =
                settings_manager_.GetPluginSettings(profile_id_, plugin_guid_);
            auto full_settings = nlohmann::json::parse(full_settings_str);

            // Filter to whitelist-only keys
            nlohmann::json filtered = nlohmann::json::object();
            for (const auto& key : whitelist_)
            {
                if (full_settings.contains(key))
                {
                    filtered[key] = full_settings[key];
                }
            }

            auto output_str = filtered.dump(indent);
            return CreateIDasReadOnlyStringFromUtf8(
                output_str.c_str(),
                pp_out_string);
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_INVALID_JSON;
        }
        catch (const std::bad_alloc& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_OUT_OF_MEMORY;
        }
    }

    DasResult DasAutoFlushJsonImpl::Clear() { return DAS_E_PERMISSION_DENIED; }

} // namespace Das::Http
