#include "DasAutoFlushJsonImpl.h"
#include "DasHttpJson.h"

#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <nlohmann/json.hpp>

using Das::Utils::ToString;
using Das::Utils::ToU8StringWithoutOwnership;

namespace Das::Http
{

    using namespace Das::ExportInterface;

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
                return DAS_TYPE_INT;
            case nlohmann::json::value_t::number_unsigned:
                return DAS_TYPE_UINT;
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

        nlohmann::json ExtractJsonFromIDasJson(IDasJson* p_json)
        {
            if (!p_json)
            {
                return {};
            }
            IDasReadOnlyString* p_str = nullptr;
            auto                result = p_json->ToString(-1, &p_str);
            if (DAS::IsFailed(result) || !p_str)
            {
                return {};
            }
            std::string json_str = ToString(p_str);
            p_str->Release();
            try
            {
                return nlohmann::json::parse(json_str);
            }
            catch (const nlohmann::json::exception&)
            {
                return {};
            }
        }

        DasResult CreateReadOnlyString(
            const char*                 str,
            DasPtr<IDasReadOnlyString>& out)
        {
            return CreateIDasReadOnlyStringFromUtf8(str, out.Put());
        }

    } // namespace

    DasAutoFlushJsonImpl::DasAutoFlushJsonImpl(
        IDasSettingsService&            settings_service,
        std::string                     profile_id,
        std::string                     plugin_guid,
        std::unordered_set<std::string> whitelist,
        std::string                     path_prefix)
        : settings_service_{settings_service},
          profile_id_{std::move(profile_id)},
          plugin_guid_{std::move(plugin_guid)},
          whitelist_{std::move(whitelist)}, path_prefix_{std::move(path_prefix)}
    {
    }

    // ── Private helpers ──

    std::string DasAutoFlushJsonImpl::MakeFullPath(const std::string& key) const
    {
        if (path_prefix_.empty())
        {
            return key;
        }
        return path_prefix_ + "." + key;
    }

    bool DasAutoFlushJsonImpl::IsPrefixAllowed(const std::string& prefix) const
    {
        const std::string prefix_dot = prefix + ".";
        for (const auto& item : whitelist_)
        {
            if (item.size() > prefix_dot.size()
                && item.compare(0, prefix_dot.size(), prefix_dot) == 0)
            {
                return true;
            }
        }
        return false;
    }

    DasResult DasAutoFlushJsonImpl::CheckWhitelist(
        IDasReadOnlyString* key,
        std::string&        out_key)
    {
        DAS_UTILS_CHECK_POINTER(key)

        const auto expected = ToU8StringWithoutOwnership(key);
        if (!expected)
        {
            return expected.error();
        }
        out_key = expected.value();

        const auto full_path = MakeFullPath(out_key);
        if (whitelist_.find(full_path) == whitelist_.end())
        {
            DAS_CORE_LOG_ERROR(
                "Field '{}' is not in whitelist. Access denied.",
                full_path);
            return DAS_E_ACCESS_DENIED;
        }
        return DAS_S_OK;
    }

    nlohmann::json DasAutoFlushJsonImpl::GetField(const std::string& full_path)
    {
        DasPtr<IDasReadOnlyString> p_profile_id;
        if (DAS::IsFailed(
                CreateReadOnlyString(profile_id_.c_str(), p_profile_id)))
        {
            return {};
        }
        DasPtr<IDasReadOnlyString> p_guid;
        if (DAS::IsFailed(CreateReadOnlyString(plugin_guid_.c_str(), p_guid)))
        {
            return {};
        }
        DasPtr<IDasReadOnlyString> p_field;
        if (DAS::IsFailed(CreateReadOnlyString(full_path.c_str(), p_field)))
        {
            return {};
        }

        DasPtr<IDasJson> json_result;
        auto             result = settings_service_.GetPluginSettingsField(
            p_profile_id.Get(),
            p_guid.Get(),
            p_field.Get(),
            json_result.Put());
        if (DAS::IsFailed(result))
        {
            return {};
        }
        return ExtractJsonFromIDasJson(json_result.Get());
    }

    DasResult DasAutoFlushJsonImpl::SetField(
        const std::string&    full_path,
        const nlohmann::json& value)
    {
        DasPtr<IDasReadOnlyString> p_profile_id;
        auto cr = CreateReadOnlyString(profile_id_.c_str(), p_profile_id);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }
        DasPtr<IDasReadOnlyString> p_guid;
        cr = CreateReadOnlyString(plugin_guid_.c_str(), p_guid);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }
        DasPtr<IDasReadOnlyString> p_field;
        cr = CreateReadOnlyString(full_path.c_str(), p_field);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }

        DasPtr<IDasJson> json_value =
            DasPtr<IDasJson>::Attach(DasHttpJson::MakeRaw(value));

        return settings_service_.UpdatePluginSettingsField(
            p_profile_id.Get(),
            p_guid.Get(),
            p_field.Get(),
            json_value.Get());
    }

    nlohmann::json DasAutoFlushJsonImpl::GetCurrentJson()
    {
        DasPtr<IDasReadOnlyString> p_profile_id;
        if (DAS::IsFailed(
                CreateReadOnlyString(profile_id_.c_str(), p_profile_id)))
        {
            return {};
        }
        DasPtr<IDasReadOnlyString> p_guid;
        if (DAS::IsFailed(CreateReadOnlyString(plugin_guid_.c_str(), p_guid)))
        {
            return {};
        }

        if (path_prefix_.empty())
        {
            DasPtr<IDasJson> json_result;
            auto             result = settings_service_.GetPluginSettings(
                p_profile_id.Get(),
                p_guid.Get(),
                json_result.Put());
            if (DAS::IsFailed(result))
            {
                return {};
            }
            return ExtractJsonFromIDasJson(json_result.Get());
        }

        DasPtr<IDasReadOnlyString> p_field;
        if (DAS::IsFailed(CreateReadOnlyString(path_prefix_.c_str(), p_field)))
        {
            return {};
        }

        DasPtr<IDasJson> json_result;
        auto             result = settings_service_.GetPluginSettingsField(
            p_profile_id.Get(),
            p_guid.Get(),
            p_field.Get(),
            json_result.Put());
        if (DAS::IsFailed(result))
        {
            return {};
        }
        return ExtractJsonFromIDasJson(json_result.Get());
    }

    // ── GetByName ──

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

        auto field = GetField(MakeFullPath(u8_key));
        if (field.is_null())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            *p_out_int = field.get<int64_t>();
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

        auto field = GetField(MakeFullPath(u8_key));
        if (field.is_null())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            *p_out_float = field.get<float>();
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

        auto field = GetField(MakeFullPath(u8_key));
        if (field.is_null())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            return CreateIDasReadOnlyStringFromUtf8(
                field.get_ref<const std::string&>().c_str(),
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

        auto field = GetField(MakeFullPath(u8_key));
        if (field.is_null())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            *p_out_bool = field.get<bool>();
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
        const auto  expected = ToU8StringWithoutOwnership(key);
        if (!expected)
        {
            return expected.error();
        }
        u8_key = expected.value();

        const auto full_path = MakeFullPath(u8_key);

        const bool is_exact = whitelist_.find(full_path) != whitelist_.end();
        const bool is_prefix = IsPrefixAllowed(full_path);
        if (!is_exact && !is_prefix)
        {
            DAS_CORE_LOG_ERROR(
                "Field '{}' is not in whitelist. Access denied.",
                full_path);
            return DAS_E_ACCESS_DENIED;
        }

        auto field = GetField(full_path);
        if (field.is_null())
        {
            return DAS_E_NOT_FOUND;
        }

        if (!field.is_object() && !field.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }

        try
        {
            auto* sub = new DasAutoFlushJsonImpl(
                settings_service_,
                profile_id_,
                plugin_guid_,
                whitelist_,
                full_path);
            sub->AddRef();
            *pp_out_das_json = sub;
            return DAS_S_OK;
        }
        catch (const std::bad_alloc& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_OUT_OF_MEMORY;
        }
    }

    // ── SetByName ──

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

        return SetField(MakeFullPath(u8_key), in_int);
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

        return SetField(MakeFullPath(u8_key), in_float);
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

        return SetField(
            MakeFullPath(u8_key),
            std::string(expected_value.value()));
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

        return SetField(MakeFullPath(u8_key), in_bool);
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

        try
        {
            auto json_val = nlohmann::json::parse(expected_value.value());
            return SetField(MakeFullPath(u8_key), json_val);
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_INVALID_JSON;
        }
    }

    // ── GetByIndex ──

    DasResult DasAutoFlushJsonImpl::GetIntByIndex(
        size_t   index,
        int64_t* p_out_int)
    {
        DAS_UTILS_CHECK_POINTER(p_out_int)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json = GetCurrentJson();
        if (!json.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }
        if (index >= json.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        try
        {
            *p_out_int = json[index].get<int64_t>();
            return DAS_S_OK;
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_TYPE_ERROR;
        }
    }

    DasResult DasAutoFlushJsonImpl::GetFloatByIndex(
        size_t index,
        float* p_out_float)
    {
        DAS_UTILS_CHECK_POINTER(p_out_float)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json = GetCurrentJson();
        if (!json.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }
        if (index >= json.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        try
        {
            *p_out_float = json[index].get<float>();
            return DAS_S_OK;
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_TYPE_ERROR;
        }
    }

    DasResult DasAutoFlushJsonImpl::GetStringByIndex(
        size_t               index,
        IDasReadOnlyString** pp_out_string)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_string)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json = GetCurrentJson();
        if (!json.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }
        if (index >= json.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        try
        {
            return CreateIDasReadOnlyStringFromUtf8(
                json[index].get_ref<const std::string&>().c_str(),
                pp_out_string);
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_TYPE_ERROR;
        }
    }

    DasResult DasAutoFlushJsonImpl::GetBoolByIndex(
        size_t index,
        bool*  p_out_bool)
    {
        DAS_UTILS_CHECK_POINTER(p_out_bool)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json = GetCurrentJson();
        if (!json.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }
        if (index >= json.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        try
        {
            *p_out_bool = json[index].get<bool>();
            return DAS_S_OK;
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_TYPE_ERROR;
        }
    }

    DasResult DasAutoFlushJsonImpl::GetObjectRefByIndex(
        size_t     index,
        IDasJson** pp_out_das_json)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_das_json)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json = GetCurrentJson();
        if (!json.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }
        if (index >= json.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        auto& element = json[index];
        if (!element.is_object() && !element.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }

        try
        {
            auto* sub = new DasAutoFlushJsonImpl(
                settings_service_,
                profile_id_,
                plugin_guid_,
                whitelist_,
                path_prefix_ + "." + std::to_string(index));
            sub->AddRef();
            *pp_out_das_json = sub;
            return DAS_S_OK;
        }
        catch (const std::bad_alloc& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_OUT_OF_MEMORY;
        }
    }

    // ── SetByIndex ──

    DasResult DasAutoFlushJsonImpl::SetIntByIndex(size_t index, int64_t in_int)
    {
        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json = GetCurrentJson();
        if (!json.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }
        if (index >= json.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        json[index] = in_int;
        return SetField(path_prefix_, json);
    }

    DasResult DasAutoFlushJsonImpl::SetFloatByIndex(
        size_t index,
        float  in_float)
    {
        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json = GetCurrentJson();
        if (!json.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }
        if (index >= json.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        json[index] = in_float;
        return SetField(path_prefix_, json);
    }

    DasResult DasAutoFlushJsonImpl::SetStringByIndex(
        size_t              index,
        IDasReadOnlyString* p_in_string)
    {
        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        DAS_UTILS_CHECK_POINTER(p_in_string)

        auto json = GetCurrentJson();
        if (!json.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }
        if (index >= json.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        const auto expected = ToU8StringWithoutOwnership(p_in_string);
        if (!expected)
        {
            return expected.error();
        }

        json[index] = expected.value();
        return SetField(path_prefix_, json);
    }

    DasResult DasAutoFlushJsonImpl::SetBoolByIndex(size_t index, bool in_bool)
    {
        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json = GetCurrentJson();
        if (!json.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }
        if (index >= json.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        json[index] = in_bool;
        return SetField(path_prefix_, json);
    }

    DasResult DasAutoFlushJsonImpl::SetObjectByIndex(
        size_t    index,
        IDasJson* p_in_das_json)
    {
        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        DAS_UTILS_CHECK_POINTER(p_in_das_json)

        auto json = GetCurrentJson();
        if (!json.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }
        if (index >= json.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

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

        try
        {
            json[index] = nlohmann::json::parse(expected_value.value());
            return SetField(path_prefix_, json);
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_INVALID_JSON;
        }
    }

    // ── GetType ──

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

        auto field = GetField(MakeFullPath(u8_key));
        if (field.is_null())
        {
            *p_out_type = DAS_TYPE_NULL;
            return DAS_S_OK;
        }

        *p_out_type = ToDasType(field.type());
        return DAS_S_OK;
    }

    DasResult DasAutoFlushJsonImpl::GetTypeByIndex(
        size_t   index,
        DasType* p_out_type)
    {
        DAS_UTILS_CHECK_POINTER(p_out_type)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json = GetCurrentJson();
        if (!json.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }
        if (index >= json.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        *p_out_type = ToDasType(json[index].type());
        return DAS_S_OK;
    }

    // ── GetSize ──

    DasResult DasAutoFlushJsonImpl::GetSize(uint64_t* p_out_size)
    {
        DAS_UTILS_CHECK_POINTER(p_out_size)

        if (path_prefix_.empty())
        {
            size_t count = 0;
            for (const auto& key : whitelist_)
            {
                auto field = GetField(key);
                if (!field.is_null())
                {
                    ++count;
                }
            }
            *p_out_size = static_cast<uint64_t>(count);
            return DAS_S_OK;
        }

        auto json = GetCurrentJson();
        *p_out_size = static_cast<uint64_t>(json.size());
        return DAS_S_OK;
    }

    // ── ToString ──

    DasResult DasAutoFlushJsonImpl::ToString(
        int32_t              indent,
        IDasReadOnlyString** pp_out_string)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_string)

        try
        {
            if (path_prefix_.empty())
            {
                nlohmann::json filtered = nlohmann::json::object();
                for (const auto& key : whitelist_)
                {
                    auto field = GetField(key);
                    if (!field.is_null())
                    {
                        // Navigate dot-path to set in filtered output
                        auto*  current = &filtered;
                        size_t start = 0;
                        size_t end = key.find('.');
                        while (end != std::string::npos)
                        {
                            auto seg = key.substr(start, end - start);
                            if (!current->contains(seg))
                            {
                                (*current)[seg] = nlohmann::json::object();
                            }
                            current = &(*current)[seg];
                            start = end + 1;
                            end = key.find('.', start);
                        }
                        (*current)[key.substr(start)] = field;
                    }
                }
                auto output_str = filtered.dump(indent);
                return CreateIDasReadOnlyStringFromUtf8(
                    output_str.c_str(),
                    pp_out_string);
            }

            auto json = GetCurrentJson();
            auto output_str = json.dump(indent);
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

    // ── Clear ──

    DasResult DasAutoFlushJsonImpl::Clear() { return DAS_E_ACCESS_DENIED; }

} // namespace Das::Http
