#pragma once

#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/DasJson.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasJson.Implements.hpp>
#include <nlohmann/json.hpp>
#include <string>

namespace Das::Http
{

    using namespace Das::ExportInterface;
    using Das::Utils::ToU8StringWithoutOwnership;

    namespace Details
    {

        inline DasType ToDasType(nlohmann::json::value_t type)
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

    } // namespace Details

    class DasHttpJson final : public DasJsonImplBase<DasHttpJson>
    {
    public:
        DasHttpJson() = default;

        explicit DasHttpJson(nlohmann::json json) : json_(std::move(json)) {}

        explicit DasHttpJson(const char* json_string)
            : json_(nlohmann::json::parse(json_string))
        {
        }

        // ── GetByName ──

        DAS_IMPL GetIntByName(IDasReadOnlyString* key, int64_t* p_out_int)
            override
        {
            DAS_UTILS_CHECK_POINTER(key)
            DAS_UTILS_CHECK_POINTER(p_out_int)

            const auto expected = ToU8StringWithoutOwnership(key);
            if (!expected)
            {
                return expected.error();
            }
            try
            {
                *p_out_int = json_.at(expected.value()).get<int64_t>();
                return DAS_S_OK;
            }
            catch (const nlohmann::json::out_of_range&)
            {
                return DAS_E_NOT_FOUND;
            }
            catch (const nlohmann::json::type_error& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_TYPE_ERROR;
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_INVALID_JSON;
            }
        }

        DAS_IMPL GetFloatByName(IDasReadOnlyString* key, float* p_out_float)
            override
        {
            DAS_UTILS_CHECK_POINTER(key)
            DAS_UTILS_CHECK_POINTER(p_out_float)

            const auto expected = ToU8StringWithoutOwnership(key);
            if (!expected)
            {
                return expected.error();
            }
            try
            {
                *p_out_float = json_.at(expected.value()).get<float>();
                return DAS_S_OK;
            }
            catch (const nlohmann::json::out_of_range&)
            {
                return DAS_E_NOT_FOUND;
            }
            catch (const nlohmann::json::type_error& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_TYPE_ERROR;
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_INVALID_JSON;
            }
        }

        DAS_IMPL GetStringByName(
            IDasReadOnlyString*  key,
            IDasReadOnlyString** pp_out_string) override
        {
            DAS_UTILS_CHECK_POINTER(key)
            DAS_UTILS_CHECK_POINTER(pp_out_string)

            const auto expected = ToU8StringWithoutOwnership(key);
            if (!expected)
            {
                return expected.error();
            }
            try
            {
                return CreateIDasReadOnlyStringFromUtf8(
                    json_.at(expected.value())
                        .get_ref<const std::string&>()
                        .c_str(),
                    pp_out_string);
            }
            catch (const nlohmann::json::out_of_range&)
            {
                return DAS_E_NOT_FOUND;
            }
            catch (const nlohmann::json::type_error& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_TYPE_ERROR;
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_INVALID_JSON;
            }
        }

        DAS_IMPL GetBoolByName(IDasReadOnlyString* key, bool* p_out_bool)
            override
        {
            DAS_UTILS_CHECK_POINTER(key)
            DAS_UTILS_CHECK_POINTER(p_out_bool)

            const auto expected = ToU8StringWithoutOwnership(key);
            if (!expected)
            {
                return expected.error();
            }
            try
            {
                *p_out_bool = json_.at(expected.value()).get<bool>();
                return DAS_S_OK;
            }
            catch (const nlohmann::json::out_of_range&)
            {
                return DAS_E_NOT_FOUND;
            }
            catch (const nlohmann::json::type_error& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_TYPE_ERROR;
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_INVALID_JSON;
            }
        }

        DAS_IMPL GetObjectRefByName(
            IDasReadOnlyString* key,
            IDasJson**          pp_out_das_json) override
        {
            DAS_UTILS_CHECK_POINTER(key)
            DAS_UTILS_CHECK_POINTER(pp_out_das_json)

            const auto expected = ToU8StringWithoutOwnership(key);
            if (!expected)
            {
                return expected.error();
            }
            try
            {
                auto& element = json_.at(expected.value());
                if (!element.is_object() && !element.is_array())
                {
                    return DAS_E_TYPE_ERROR;
                }
                auto* sub = new DasHttpJson(element);
                sub->AddRef();
                *pp_out_das_json = sub;
                return DAS_S_OK;
            }
            catch (const nlohmann::json::out_of_range&)
            {
                return DAS_E_NOT_FOUND;
            }
            catch (const nlohmann::json::type_error& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_TYPE_ERROR;
            }
            catch (const std::bad_alloc& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_OUT_OF_MEMORY;
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_INVALID_JSON;
            }
        }

        // ── SetByName ──

        DAS_IMPL SetIntByName(IDasReadOnlyString* key, int64_t in_int) override
        {
            DAS_UTILS_CHECK_POINTER(key)

            const auto expected = ToU8StringWithoutOwnership(key);
            if (!expected)
            {
                return expected.error();
            }
            json_[expected.value()] = in_int;
            return DAS_S_OK;
        }

        DAS_IMPL SetFloatByName(IDasReadOnlyString* key, float in_float)
            override
        {
            DAS_UTILS_CHECK_POINTER(key)

            const auto expected = ToU8StringWithoutOwnership(key);
            if (!expected)
            {
                return expected.error();
            }
            json_[expected.value()] = in_float;
            return DAS_S_OK;
        }

        DAS_IMPL SetStringByName(
            IDasReadOnlyString* key,
            IDasReadOnlyString* p_in_string) override
        {
            DAS_UTILS_CHECK_POINTER(key)
            DAS_UTILS_CHECK_POINTER(p_in_string)

            const auto expected_key = ToU8StringWithoutOwnership(key);
            if (!expected_key)
            {
                return expected_key.error();
            }
            const auto expected_value = ToU8StringWithoutOwnership(p_in_string);
            if (!expected_value)
            {
                return expected_value.error();
            }
            json_[expected_key.value()] = expected_value.value();
            return DAS_S_OK;
        }

        DAS_IMPL SetBoolByName(IDasReadOnlyString* key, bool in_bool) override
        {
            DAS_UTILS_CHECK_POINTER(key)

            const auto expected = ToU8StringWithoutOwnership(key);
            if (!expected)
            {
                return expected.error();
            }
            json_[expected.value()] = in_bool;
            return DAS_S_OK;
        }

        DAS_IMPL SetObjectByName(
            IDasReadOnlyString* key,
            IDasJson*           p_in_das_json) override
        {
            DAS_UTILS_CHECK_POINTER(key)
            DAS_UTILS_CHECK_POINTER(p_in_das_json)

            const auto expected_key = ToU8StringWithoutOwnership(key);
            if (!expected_key)
            {
                return expected_key.error();
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
                json_[expected_key.value()] =
                    nlohmann::json::parse(expected_value.value());
                return DAS_S_OK;
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_INVALID_JSON;
            }
        }

        // ── GetByIndex ──

        DAS_IMPL GetIntByIndex(size_t index, int64_t* p_out_int) override
        {
            DAS_UTILS_CHECK_POINTER(p_out_int)

            if (!json_.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            if (index >= json_.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }
            try
            {
                *p_out_int = json_[index].get<int64_t>();
                return DAS_S_OK;
            }
            catch (const nlohmann::json::type_error& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_TYPE_ERROR;
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_INVALID_JSON;
            }
        }

        DAS_IMPL GetFloatByIndex(size_t index, float* p_out_float) override
        {
            DAS_UTILS_CHECK_POINTER(p_out_float)

            if (!json_.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            if (index >= json_.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }
            try
            {
                *p_out_float = json_[index].get<float>();
                return DAS_S_OK;
            }
            catch (const nlohmann::json::type_error& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_TYPE_ERROR;
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_INVALID_JSON;
            }
        }

        DAS_IMPL GetStringByIndex(
            size_t               index,
            IDasReadOnlyString** pp_out_string) override
        {
            DAS_UTILS_CHECK_POINTER(pp_out_string)

            if (!json_.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            if (index >= json_.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }
            try
            {
                return CreateIDasReadOnlyStringFromUtf8(
                    json_[index].get_ref<const std::string&>().c_str(),
                    pp_out_string);
            }
            catch (const nlohmann::json::type_error& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_TYPE_ERROR;
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_INVALID_JSON;
            }
        }

        DAS_IMPL GetBoolByIndex(size_t index, bool* p_out_bool) override
        {
            DAS_UTILS_CHECK_POINTER(p_out_bool)

            if (!json_.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            if (index >= json_.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }
            try
            {
                *p_out_bool = json_[index].get<bool>();
                return DAS_S_OK;
            }
            catch (const nlohmann::json::type_error& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_TYPE_ERROR;
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_INVALID_JSON;
            }
        }

        DAS_IMPL GetObjectRefByIndex(size_t index, IDasJson** pp_out_das_json)
            override
        {
            DAS_UTILS_CHECK_POINTER(pp_out_das_json)

            if (!json_.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            if (index >= json_.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }

            auto& element = json_[index];
            if (!element.is_object() && !element.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }

            try
            {
                auto* sub = new DasHttpJson(element);
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

        DAS_IMPL SetIntByIndex(size_t index, int64_t in_int) override
        {
            if (!json_.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            if (index >= json_.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }
            json_[index] = in_int;
            return DAS_S_OK;
        }

        DAS_IMPL SetFloatByIndex(size_t index, float in_float) override
        {
            if (!json_.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            if (index >= json_.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }
            json_[index] = in_float;
            return DAS_S_OK;
        }

        DAS_IMPL SetStringByIndex(size_t index, IDasReadOnlyString* p_in_string)
            override
        {
            DAS_UTILS_CHECK_POINTER(p_in_string)

            if (!json_.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            if (index >= json_.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }

            const auto expected = ToU8StringWithoutOwnership(p_in_string);
            if (!expected)
            {
                return expected.error();
            }
            json_[index] = expected.value();
            return DAS_S_OK;
        }

        DAS_IMPL SetBoolByIndex(size_t index, bool in_bool) override
        {
            if (!json_.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            if (index >= json_.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }
            json_[index] = in_bool;
            return DAS_S_OK;
        }

        DAS_IMPL SetObjectByIndex(size_t index, IDasJson* p_in_das_json)
            override
        {
            DAS_UTILS_CHECK_POINTER(p_in_das_json)

            if (!json_.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            if (index >= json_.size())
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
                json_[index] = nlohmann::json::parse(expected_value.value());
                return DAS_S_OK;
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_CORE_LOG_EXCEPTION(ex);
                return DAS_E_INVALID_JSON;
            }
        }

        // ── Type/Size/ToString/Clear ──

        DAS_IMPL GetTypeByName(IDasReadOnlyString* key, DasType* p_out_type)
            override
        {
            DAS_UTILS_CHECK_POINTER(key)
            DAS_UTILS_CHECK_POINTER(p_out_type)

            const auto expected = ToU8StringWithoutOwnership(key);
            if (!expected)
            {
                return expected.error();
            }

            auto it = json_.find(expected.value());
            if (it == json_.end())
            {
                *p_out_type = DAS_TYPE_NULL;
                return DAS_S_OK;
            }
            *p_out_type = Details::ToDasType(it->type());
            return DAS_S_OK;
        }

        DAS_IMPL GetTypeByIndex(size_t index, DasType* p_out_type) override
        {
            DAS_UTILS_CHECK_POINTER(p_out_type)

            if (!json_.is_array())
            {
                return DAS_E_TYPE_ERROR;
            }
            if (index >= json_.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }
            *p_out_type = Details::ToDasType(json_[index].type());
            return DAS_S_OK;
        }

        DAS_IMPL GetSize(uint64_t* p_out_size) override
        {
            DAS_UTILS_CHECK_POINTER(p_out_size)
            *p_out_size = static_cast<uint64_t>(json_.size());
            return DAS_S_OK;
        }

        DAS_IMPL ToString(int32_t indent, IDasReadOnlyString** pp_out_string)
            override
        {
            DAS_UTILS_CHECK_POINTER(pp_out_string)
            try
            {
                auto output_str = json_.dump(indent);
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

        DAS_IMPL Clear() override
        {
            json_ = nlohmann::json::object();
            return DAS_S_OK;
        }

    private:
        nlohmann::json json_;
    };

} // namespace Das::Http
