#include <cassert>

#include "DasAutoFlushJsonImpl.h"
#include "DasHttpJson.h"

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>

using Das::Utils::ToString;

namespace Das::Http
{

    using namespace Das::ExportInterface;

    namespace
    {

        std::optional<yyjson::writer::detail::value> ExtractJsonFromIDasJson(
            IDasJson* p_json)
        {
            if (!p_json)
            {
                return std::nullopt;
            }
            DasPtr<IDasReadOnlyString> p_str;
            auto result = p_json->ToString(-1, p_str.Put());
            if (DAS::IsFailed(result))
            {
                return std::nullopt;
            }
            std::string json_str = ToString(p_str.Get());
            return Das::Utils::ParseYyjsonFromString(json_str);
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
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)

        const char* expected = nullptr;
        auto        cr = key->GetUtf8(&expected);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }
        out_key = expected;

        const auto full_path = MakeFullPath(out_key);
        if (whitelist_.find(full_path) == whitelist_.end())
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "Field '{}' is not in whitelist. Access denied.",
                    full_path)
                    .c_str());
            return DAS_E_ACCESS_DENIED;
        }
        return DAS_S_OK;
    }

    std::optional<yyjson::writer::detail::value> DasAutoFlushJsonImpl::GetField(
        const std::string& full_path)
    {
        DasPtr<IDasReadOnlyString> p_profile_id;
        if (DAS::IsFailed(
                CreateReadOnlyString(profile_id_.c_str(), p_profile_id)))
        {
            return std::nullopt;
        }
        DasGuid guid;
        if (DAS::IsFailed(DasMakeDasGuid(plugin_guid_.c_str(), &guid)))
        {
            return std::nullopt;
        }
        DasPtr<IDasReadOnlyString> p_field;
        if (DAS::IsFailed(CreateReadOnlyString(full_path.c_str(), p_field)))
        {
            return std::nullopt;
        }

        DasPtr<IDasJson> json_result;
        auto             result = settings_service_.GetPluginSettingsField(
            p_profile_id.Get(),
            &guid,
            p_field.Get(),
            json_result.Put());
        if (DAS::IsFailed(result))
        {
            return std::nullopt;
        }
        return ExtractJsonFromIDasJson(json_result.Get());
    }

    DasResult DasAutoFlushJsonImpl::SetField(
        const std::string&                   full_path,
        const yyjson::writer::detail::value& value)
    {
        DasPtr<IDasReadOnlyString> p_profile_id;
        auto cr = CreateReadOnlyString(profile_id_.c_str(), p_profile_id);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }
        DasGuid guid;
        cr = DasMakeDasGuid(plugin_guid_.c_str(), &guid);
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

        // Serialize yyjson value to create IDasJson
        auto serialized = Das::Utils::SerializeYyjsonValue(value);
        if (!serialized)
        {
            return DAS_E_INVALID_JSON;
        }

        auto* p_json_value = new DasHttpJson(serialized->c_str());
        p_json_value->AddRef();
        DasPtr<IDasJson> json_value = DasPtr<IDasJson>::Attach(p_json_value);

        return settings_service_.UpdatePluginSettingsField(
            p_profile_id.Get(),
            &guid,
            p_field.Get(),
            json_value.Get());
    }

    std::optional<yyjson::writer::detail::value>
    DasAutoFlushJsonImpl::GetCurrentJson()
    {
        DasPtr<IDasReadOnlyString> p_profile_id;
        if (DAS::IsFailed(
                CreateReadOnlyString(profile_id_.c_str(), p_profile_id)))
        {
            return std::nullopt;
        }
        DasGuid guid;
        if (DAS::IsFailed(DasMakeDasGuid(plugin_guid_.c_str(), &guid)))
        {
            return std::nullopt;
        }

        if (path_prefix_.empty())
        {
            DasPtr<IDasJson> json_result;
            auto             result = settings_service_.GetPluginSettings(
                p_profile_id.Get(),
                &guid,
                json_result.Put());
            if (DAS::IsFailed(result))
            {
                return std::nullopt;
            }
            return ExtractJsonFromIDasJson(json_result.Get());
        }

        DasPtr<IDasReadOnlyString> p_field;
        if (DAS::IsFailed(CreateReadOnlyString(path_prefix_.c_str(), p_field)))
        {
            return std::nullopt;
        }

        DasPtr<IDasJson> json_result;
        auto             result = settings_service_.GetPluginSettingsField(
            p_profile_id.Get(),
            &guid,
            p_field.Get(),
            json_result.Put());
        if (DAS::IsFailed(result))
        {
            return std::nullopt;
        }
        return ExtractJsonFromIDasJson(json_result.Get());
    }

    // ── GetByName ──

    DasResult DasAutoFlushJsonImpl::GetIntByName(
        IDasReadOnlyString* key,
        int64_t*            p_out_int)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_int)

        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_opt = GetField(MakeFullPath(u8_key));
        if (!field_opt || field_opt->is_null())
        {
            return DAS_E_NOT_FOUND;
        }

        auto opt = field_opt->as_sint();
        if (!opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        *p_out_int = opt.value();
        return DAS_S_OK;
    }

    DasResult DasAutoFlushJsonImpl::GetFloatByName(
        IDasReadOnlyString* key,
        float*              p_out_float)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_float)

        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_opt = GetField(MakeFullPath(u8_key));
        if (!field_opt || field_opt->is_null())
        {
            return DAS_E_NOT_FOUND;
        }

        auto opt = field_opt->as_real();
        if (!opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        *p_out_float = static_cast<float>(opt.value());
        return DAS_S_OK;
    }

    DasResult DasAutoFlushJsonImpl::GetStringByName(
        IDasReadOnlyString*  key,
        IDasReadOnlyString** pp_out_string)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_string)

        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_opt = GetField(MakeFullPath(u8_key));
        if (!field_opt || field_opt->is_null())
        {
            return DAS_E_NOT_FOUND;
        }

        auto opt = field_opt->as_string();
        if (!opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        std::string str_val(opt.value());
        return CreateIDasReadOnlyStringFromUtf8(str_val.c_str(), pp_out_string);
    }

    DasResult DasAutoFlushJsonImpl::GetBoolByName(
        IDasReadOnlyString* key,
        bool*               p_out_bool)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_bool)

        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_opt = GetField(MakeFullPath(u8_key));
        if (!field_opt || field_opt->is_null())
        {
            return DAS_E_NOT_FOUND;
        }

        auto opt = field_opt->as_bool();
        if (!opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        *p_out_bool = opt.value();
        return DAS_S_OK;
    }

    DasResult DasAutoFlushJsonImpl::GetObjectRefByName(
        IDasReadOnlyString* key,
        IDasJson**          pp_out_das_json)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_das_json)

        std::string u8_key;
        const char* expected = nullptr;
        auto        cr = key->GetUtf8(&expected);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }
        u8_key = expected;

        const auto full_path = MakeFullPath(u8_key);

        const bool is_exact = whitelist_.find(full_path) != whitelist_.end();
        const bool is_prefix = IsPrefixAllowed(full_path);
        if (!is_exact && !is_prefix)
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "Field '{}' is not in whitelist. Access denied.",
                    full_path)
                    .c_str());
            return DAS_E_ACCESS_DENIED;
        }

        auto field_opt = GetField(full_path);
        if (!field_opt || field_opt->is_null())
        {
            return DAS_E_NOT_FOUND;
        }

        if (!field_opt->is_object() && !field_opt->is_array())
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
            DAS_LOG_ERROR(ex.what());
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

        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_in_string)

        const char* value_str = nullptr;
        auto        cr = p_in_string->GetUtf8(&value_str);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }

        return SetField(MakeFullPath(u8_key), std::string(value_str));
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

        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_in_das_json)

        DasPtr<IDasReadOnlyString> p_json_str;
        auto to_string_result = p_in_das_json->ToString(-1, p_json_str.Put());
        if (DAS::IsFailed(to_string_result))
        {
            return to_string_result;
        }

        const char* value_str = nullptr;
        auto        cr = p_json_str->GetUtf8(&value_str);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }

        auto parsed = Das::Utils::ParseYyjsonFromString(value_str);
        if (!parsed)
        {
            return DAS_E_INVALID_JSON;
        }

        return SetField(MakeFullPath(u8_key), parsed.value());
    }

    // ── GetByIndex ──

    DasResult DasAutoFlushJsonImpl::GetIntByIndex(
        size_t   index,
        int64_t* p_out_int)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_int)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            return DAS_E_NOT_FOUND;
        }

        auto arr_opt = json_opt->as_array();
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        const auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        auto opt = arr[index].as_sint();
        if (!opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        *p_out_int = opt.value();
        return DAS_S_OK;
    }

    DasResult DasAutoFlushJsonImpl::GetFloatByIndex(
        size_t index,
        float* p_out_float)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_float)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            return DAS_E_NOT_FOUND;
        }

        auto arr_opt = json_opt->as_array();
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        const auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        auto opt = arr[index].as_real();
        if (!opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        *p_out_float = static_cast<float>(opt.value());
        return DAS_S_OK;
    }

    DasResult DasAutoFlushJsonImpl::GetStringByIndex(
        size_t               index,
        IDasReadOnlyString** pp_out_string)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_string)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            return DAS_E_NOT_FOUND;
        }

        auto arr_opt = json_opt->as_array();
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        const auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        auto opt = arr[index].as_string();
        if (!opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        std::string str_val(opt.value());
        return CreateIDasReadOnlyStringFromUtf8(str_val.c_str(), pp_out_string);
    }

    DasResult DasAutoFlushJsonImpl::GetBoolByIndex(
        size_t index,
        bool*  p_out_bool)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_bool)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            return DAS_E_NOT_FOUND;
        }

        auto arr_opt = json_opt->as_array();
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        const auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        auto opt = arr[index].as_bool();
        if (!opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        *p_out_bool = opt.value();
        return DAS_S_OK;
    }

    DasResult DasAutoFlushJsonImpl::GetObjectRefByIndex(
        size_t     index,
        IDasJson** pp_out_das_json)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_das_json)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            return DAS_E_NOT_FOUND;
        }

        auto arr_opt = json_opt->as_array();
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        const auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        const auto& val = arr[index];
        if (!val.is_object() && !val.is_array())
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
            DAS_LOG_ERROR(ex.what());
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

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            return DAS_E_NOT_FOUND;
        }

        auto arr_opt = json_opt->as_array();
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        arr[index] = in_int;
        return SetField(path_prefix_, std::move(json_opt.value()));
    }

    DasResult DasAutoFlushJsonImpl::SetFloatByIndex(
        size_t index,
        float  in_float)
    {
        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            return DAS_E_NOT_FOUND;
        }

        auto arr_opt = json_opt->as_array();
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        arr[index] = in_float;
        return SetField(path_prefix_, std::move(json_opt.value()));
    }

    DasResult DasAutoFlushJsonImpl::SetStringByIndex(
        size_t              index,
        IDasReadOnlyString* p_in_string)
    {
        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_in_string)

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            return DAS_E_NOT_FOUND;
        }

        auto arr_opt = json_opt->as_array();
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        const char* value_str = nullptr;
        auto        cr = p_in_string->GetUtf8(&value_str);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }

        arr[index] = std::string(value_str);
        return SetField(path_prefix_, std::move(json_opt.value()));
    }

    DasResult DasAutoFlushJsonImpl::SetBoolByIndex(size_t index, bool in_bool)
    {
        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            return DAS_E_NOT_FOUND;
        }

        auto arr_opt = json_opt->as_array();
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        arr[index] = in_bool;
        return SetField(path_prefix_, std::move(json_opt.value()));
    }

    DasResult DasAutoFlushJsonImpl::SetObjectByIndex(
        size_t    index,
        IDasJson* p_in_das_json)
    {
        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_in_das_json)

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            return DAS_E_NOT_FOUND;
        }

        auto arr_opt = json_opt->as_array();
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        DasPtr<IDasReadOnlyString> p_json_str;
        auto to_string_result = p_in_das_json->ToString(-1, p_json_str.Put());
        if (DAS::IsFailed(to_string_result))
        {
            return to_string_result;
        }

        const char* value_str = nullptr;
        auto        cr = p_json_str->GetUtf8(&value_str);
        if (DAS::IsFailed(cr))
        {
            return cr;
        }

        auto parsed = Das::Utils::ParseYyjsonFromString(value_str);
        if (!parsed)
        {
            return DAS_E_INVALID_JSON;
        }

        arr[index] = std::move(parsed.value());
        return SetField(path_prefix_, std::move(json_opt.value()));
    }

    // ── GetType ──

    DasResult DasAutoFlushJsonImpl::GetTypeByName(
        IDasReadOnlyString* key,
        DasType*            p_out_type)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_type)

        std::string u8_key;
        auto        result = CheckWhitelist(key, u8_key);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto field_opt = GetField(MakeFullPath(u8_key));
        if (!field_opt || field_opt->is_null())
        {
            *p_out_type = DAS_TYPE_NULL;
            return DAS_S_OK;
        }

        const auto& field = field_opt.value();
        if (field.is_null())
        {
            *p_out_type = DAS_TYPE_NULL;
        }
        else if (field.is_object())
        {
            *p_out_type = DAS_TYPE_JSON_OBJECT;
        }
        else if (field.is_array())
        {
            *p_out_type = DAS_TYPE_JSON_ARRAY;
        }
        else if (field.is_string())
        {
            *p_out_type = DAS_TYPE_STRING;
        }
        else if (field.is_bool())
        {
            *p_out_type = DAS_TYPE_BOOL;
        }
        else if (field.is_uint())
        {
            *p_out_type = DAS_TYPE_UINT;
        }
        else if (field.is_sint())
        {
            *p_out_type = DAS_TYPE_INT;
        }
        else if (field.is_real())
        {
            *p_out_type = DAS_TYPE_FLOAT;
        }
        else
        {
            *p_out_type = DAS_TYPE_UNSUPPORTED;
        }
        return DAS_S_OK;
    }

    DasResult DasAutoFlushJsonImpl::GetTypeByIndex(
        size_t   index,
        DasType* p_out_type)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_type)

        if (path_prefix_.empty())
        {
            return DAS_E_TYPE_ERROR;
        }

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            return DAS_E_NOT_FOUND;
        }

        auto arr_opt = json_opt->as_array();
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        const auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        *p_out_type = Das::Utils::YyjsonValueToDasType(arr[index]);
        return DAS_S_OK;
    }

    // ── GetSize ──

    DasResult DasAutoFlushJsonImpl::GetSize(uint64_t* p_out_size)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_size)

        if (path_prefix_.empty())
        {
            size_t count = 0;
            for (const auto& key : whitelist_)
            {
                auto field_opt = GetField(key);
                if (field_opt && !field_opt->is_null())
                {
                    ++count;
                }
            }
            *p_out_size = static_cast<uint64_t>(count);
            return DAS_S_OK;
        }

        auto json_opt = GetCurrentJson();
        if (!json_opt)
        {
            *p_out_size = 0;
            return DAS_S_OK;
        }

        if (auto arr_opt = json_opt->as_array())
        {
            *p_out_size = static_cast<uint64_t>(arr_opt->size());
        }
        else if (auto obj_opt = json_opt->as_object())
        {
            *p_out_size = static_cast<uint64_t>(obj_opt->size());
        }
        else
        {
            *p_out_size = 0;
        }
        return DAS_S_OK;
    }

    // ── ToString ──

    DasResult DasAutoFlushJsonImpl::ToString(
        int32_t              indent,
        IDasReadOnlyString** pp_out_string)
    {
        DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_string)

        try
        {
            auto flags = (indent >= 0) ? yyjson::WriteFlag::Pretty
                                       : yyjson::WriteFlag::NoFlag;

            if (path_prefix_.empty())
            {
                // Build filtered output from whitelist with dot-path nesting
                auto filtered = yyjson::writer::detail::value(
                    yyjson::writer::detail::object{});
                for (const auto& key : whitelist_)
                {
                    auto field_opt = GetField(key);
                    if (!field_opt || field_opt->is_null())
                    {
                        continue;
                    }

                    auto filtered_obj_opt = filtered.as_object();
                    if (!filtered_obj_opt)
                    {
                        continue;
                    }
                    auto obj = filtered_obj_opt.value();

                    // Navigate dot-path to set in filtered output
                    size_t start = 0;
                    size_t end = key.find('.');
                    while (end != std::string::npos)
                    {
                        auto seg = key.substr(start, end - start);
                        auto child_ref = obj[seg];
                        if (!child_ref.is_object())
                        {
                            child_ref = yyjson::writer::detail::value(
                                yyjson::writer::detail::object{});
                        }
                        auto child_obj_opt = child_ref.as_object();
                        if (!child_obj_opt)
                        {
                            break;
                        }
                        obj = child_obj_opt.value();
                        start = end + 1;
                        end = key.find('.', start);
                    }
                    obj[key.substr(start)] = std::move(field_opt.value());
                }

                auto output_str = filtered.write(flags);
                return CreateIDasReadOnlyStringFromUtf8(
                    std::string(output_str.data(), output_str.size()).c_str(),
                    pp_out_string);
            }

            auto json_opt = GetCurrentJson();
            if (!json_opt)
            {
                return DAS_E_INVALID_JSON;
            }
            auto output_str = json_opt->write(flags);
            return CreateIDasReadOnlyStringFromUtf8(
                std::string(output_str.data(), output_str.size()).c_str(),
                pp_out_string);
        }
        catch (const yyjson::write_error& ex)
        {
            DAS_LOG_ERROR(ex.what());
            return DAS_E_INVALID_JSON;
        }
        catch (const std::bad_alloc& ex)
        {
            DAS_LOG_ERROR(ex.what());
            return DAS_E_OUT_OF_MEMORY;
        }
    }

    // ── Clear ──

    DasResult DasAutoFlushJsonImpl::Clear() { return DAS_E_ACCESS_DENIED; }

} // namespace Das::Http
