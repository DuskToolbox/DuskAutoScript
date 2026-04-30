#pragma once

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/DasJson.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasJson.Implements.hpp>
#include <string>

namespace Das::Http
{

    using namespace Das::ExportInterface;

    // ToDasType removed — use Das::Utils::YyjsonValueToDasType instead.

    class DasHttpJson final : public DasJsonImplBase<DasHttpJson>
    {
    public:
        DasHttpJson() = default;

        explicit DasHttpJson(yyjson::value json) : json_(std::move(json)) {}

        explicit DasHttpJson(const char* json_string)
        {
            auto parsed = Das::Utils::ParseYyjsonFromString(json_string);
            if (!parsed)
            {
                json_ = yyjson::object{};
            }
            else
            {
                json_ = std::move(parsed.value());
            }
        }

        // ── GetByName ──

        DAS_IMPL GetIntByName(IDasReadOnlyString* key, int64_t* p_out_int)
            override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_int)

            const char* key_str = nullptr;
            auto        cr = key->GetUtf8(&key_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            auto obj_opt = json_.as_object();
            if (!obj_opt)
            {
                return DAS_E_NOT_FOUND;
            }
            const auto& obj = obj_opt.value();
            auto        it = obj.find(key_str);
            if (it == obj.end())
            {
                return DAS_E_NOT_FOUND;
            }
            const auto& val = it->second;
            auto        opt = val.as_sint();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *p_out_int = opt.value();
            return DAS_S_OK;
        }

        DAS_IMPL GetFloatByName(IDasReadOnlyString* key, float* p_out_float)
            override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_float)

            const char* key_str = nullptr;
            auto        cr = key->GetUtf8(&key_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            auto obj_opt = json_.as_object();
            if (!obj_opt)
            {
                return DAS_E_NOT_FOUND;
            }
            const auto& obj = obj_opt.value();
            auto        it = obj.find(key_str);
            if (it == obj.end())
            {
                return DAS_E_NOT_FOUND;
            }
            const auto& val = it->second;
            auto        opt = val.as_real();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *p_out_float = static_cast<float>(opt.value());
            return DAS_S_OK;
        }

        DAS_IMPL GetStringByName(
            IDasReadOnlyString*  key,
            IDasReadOnlyString** pp_out_string) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_string)

            const char* key_str = nullptr;
            auto        cr = key->GetUtf8(&key_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            auto obj_opt = json_.as_object();
            if (!obj_opt)
            {
                return DAS_E_NOT_FOUND;
            }
            const auto& obj = obj_opt.value();
            auto        it = obj.find(key_str);
            if (it == obj.end())
            {
                return DAS_E_NOT_FOUND;
            }
            const auto& val = it->second;
            auto        opt = val.as_string();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            // Copy to std::string for null-terminated c_str()
            std::string str_val(opt.value());
            return CreateIDasReadOnlyStringFromUtf8(
                str_val.c_str(),
                pp_out_string);
        }

        DAS_IMPL GetBoolByName(IDasReadOnlyString* key, bool* p_out_bool)
            override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_bool)

            const char* key_str = nullptr;
            auto        cr = key->GetUtf8(&key_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            auto obj_opt = json_.as_object();
            if (!obj_opt)
            {
                return DAS_E_NOT_FOUND;
            }
            const auto& obj = obj_opt.value();
            auto        it = obj.find(key_str);
            if (it == obj.end())
            {
                return DAS_E_NOT_FOUND;
            }
            const auto& val = it->second;
            auto        opt = val.as_bool();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *p_out_bool = opt.value();
            return DAS_S_OK;
        }

        DAS_IMPL GetObjectRefByName(
            IDasReadOnlyString* key,
            IDasJson**          pp_out_das_json) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_das_json)

            const char* key_str = nullptr;
            auto        cr = key->GetUtf8(&key_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            auto obj_opt = json_.as_object();
            if (!obj_opt)
            {
                return DAS_E_NOT_FOUND;
            }
            const auto& obj = obj_opt.value();
            auto        it = obj.find(key_str);
            if (it == obj.end())
            {
                return DAS_E_NOT_FOUND;
            }
            const auto& val = it->second;
            if (!val.is_object() && !val.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            try
            {
                auto  new_val = yyjson::value(val);
                auto* sub = new DasHttpJson(std::move(new_val));
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

        DAS_IMPL SetIntByName(IDasReadOnlyString* key, int64_t in_int) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)

            const char* key_str = nullptr;
            auto        cr = key->GetUtf8(&key_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            if (json_.is_null())
            {
                json_ = yyjson::object{};
            }

            auto obj_opt = json_.as_object();
            if (!obj_opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            auto& obj = obj_opt.value();
            obj[key_str] = in_int;
            return DAS_S_OK;
        }

        DAS_IMPL SetFloatByName(IDasReadOnlyString* key, float in_float)
            override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)

            const char* key_str = nullptr;
            auto        cr = key->GetUtf8(&key_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            if (json_.is_null())
            {
                json_ = yyjson::object{};
            }

            auto obj_opt = json_.as_object();
            if (!obj_opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            auto& obj = obj_opt.value();
            obj[key_str] = in_float;
            return DAS_S_OK;
        }

        DAS_IMPL SetStringByName(
            IDasReadOnlyString* key,
            IDasReadOnlyString* p_in_string) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_in_string)

            const char* key_str = nullptr;
            auto        cr = key->GetUtf8(&key_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            const char* value_str = nullptr;
            cr = p_in_string->GetUtf8(&value_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            if (json_.is_null())
            {
                json_ = yyjson::object{};
            }

            auto obj_opt = json_.as_object();
            if (!obj_opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            auto& obj = obj_opt.value();
            obj[key_str] = std::string(value_str);
            return DAS_S_OK;
        }

        DAS_IMPL SetBoolByName(IDasReadOnlyString* key, bool in_bool) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)

            const char* key_str = nullptr;
            auto        cr = key->GetUtf8(&key_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            if (json_.is_null())
            {
                json_ = yyjson::object{};
            }

            auto obj_opt = json_.as_object();
            if (!obj_opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            auto& obj = obj_opt.value();
            obj[key_str] = in_bool;
            return DAS_S_OK;
        }

        DAS_IMPL SetObjectByName(
            IDasReadOnlyString* key,
            IDasJson*           p_in_das_json) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_in_das_json)

            const char* key_str = nullptr;
            auto        cr = key->GetUtf8(&key_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            DasPtr<IDasReadOnlyString> p_json_str;
            auto                       to_string_result =
                p_in_das_json->ToString(-1, p_json_str.Put());
            if (DAS::IsFailed(to_string_result))
            {
                return to_string_result;
            }

            const char* value_str = nullptr;
            cr = p_json_str->GetUtf8(&value_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            auto parsed = Das::Utils::ParseYyjsonFromString(value_str);
            if (!parsed)
            {
                return DAS_E_INVALID_JSON;
            }

            if (json_.is_null())
            {
                json_ = yyjson::object{};
            }

            auto obj_opt = json_.as_object();
            if (!obj_opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            auto& obj = obj_opt.value();
            obj[key_str] = std::move(parsed.value());
            return DAS_S_OK;
        }

        // ── GetByIndex ──

        DAS_IMPL GetIntByIndex(size_t index, int64_t* p_out_int) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_int)

            auto arr_opt = json_.as_array();
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
            auto        opt = val.as_sint();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *p_out_int = opt.value();
            return DAS_S_OK;
        }

        DAS_IMPL GetFloatByIndex(size_t index, float* p_out_float) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_float)

            auto arr_opt = json_.as_array();
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
            auto        opt = val.as_real();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *p_out_float = static_cast<float>(opt.value());
            return DAS_S_OK;
        }

        DAS_IMPL GetStringByIndex(
            size_t               index,
            IDasReadOnlyString** pp_out_string) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_string)

            auto arr_opt = json_.as_array();
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
            auto        opt = val.as_string();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            std::string str_val(opt.value());
            return CreateIDasReadOnlyStringFromUtf8(
                str_val.c_str(),
                pp_out_string);
        }

        DAS_IMPL GetBoolByIndex(size_t index, bool* p_out_bool) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_bool)

            auto arr_opt = json_.as_array();
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
            auto        opt = val.as_bool();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *p_out_bool = opt.value();
            return DAS_S_OK;
        }

        DAS_IMPL GetObjectRefByIndex(size_t index, IDasJson** pp_out_das_json)
            override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_das_json)

            auto arr_opt = json_.as_array();
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
                auto  new_val = yyjson::value(val);
                auto* sub = new DasHttpJson(std::move(new_val));
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

        DAS_IMPL SetIntByIndex(size_t index, int64_t in_int) override
        {
            auto arr_opt = json_.as_array();
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
            return DAS_S_OK;
        }

        DAS_IMPL SetFloatByIndex(size_t index, float in_float) override
        {
            auto arr_opt = json_.as_array();
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
            return DAS_S_OK;
        }

        DAS_IMPL SetStringByIndex(size_t index, IDasReadOnlyString* p_in_string)
            override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_in_string)

            auto arr_opt = json_.as_array();
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
            return DAS_S_OK;
        }

        DAS_IMPL SetBoolByIndex(size_t index, bool in_bool) override
        {
            auto arr_opt = json_.as_array();
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
            return DAS_S_OK;
        }

        DAS_IMPL SetObjectByIndex(size_t index, IDasJson* p_in_das_json)
            override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_in_das_json)

            auto arr_opt = json_.as_array();
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
            auto                       to_string_result =
                p_in_das_json->ToString(-1, p_json_str.Put());
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
            return DAS_S_OK;
        }

        // ── Type/Size/ToString/Clear ──

        DAS_IMPL GetTypeByName(IDasReadOnlyString* key, DasType* p_out_type)
            override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(key)
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_type)

            const char* key_str = nullptr;
            auto        cr = key->GetUtf8(&key_str);
            if (DAS::IsFailed(cr))
            {
                return cr;
            }

            auto obj_opt = json_.as_object();
            if (!obj_opt)
            {
                *p_out_type = DAS_TYPE_NULL;
                return DAS_S_OK;
            }
            const auto& obj = obj_opt.value();
            auto        it = obj.find(key_str);
            if (it == obj.end())
            {
                *p_out_type = DAS_TYPE_NULL;
                return DAS_S_OK;
            }
            *p_out_type = Das::Utils::YyjsonValueToDasType(it->second);
            return DAS_S_OK;
        }

        DAS_IMPL GetTypeByIndex(size_t index, DasType* p_out_type) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_type)

            auto arr_opt = json_.as_array();
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

        DAS_IMPL GetSize(uint64_t* p_out_size) override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(p_out_size)

            if (auto obj_opt = json_.as_object())
            {
                *p_out_size = static_cast<uint64_t>(obj_opt->size());
            }
            else if (auto arr_opt = json_.as_array())
            {
                *p_out_size = static_cast<uint64_t>(arr_opt->size());
            }
            else
            {
                *p_out_size = 0;
            }
            return DAS_S_OK;
        }

        DAS_IMPL ToString(int32_t indent, IDasReadOnlyString** pp_out_string)
            override
        {
            DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_string)

            try
            {
                auto flags = (indent >= 0) ? yyjson::WriteFlag::Pretty
                                           : yyjson::WriteFlag::NoFlag;
                auto output_str = json_.write(flags);
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

        DAS_IMPL Clear() override
        {
            json_ = yyjson::object{};
            return DAS_S_OK;
        }

    private:
        yyjson::value json_;
    };

} // namespace Das::Http
