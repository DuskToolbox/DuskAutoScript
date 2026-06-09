#include <das/Core/GraphRuntime/LegacyJsonAdapter.h>

#include <das/Core/Logger/Logger.h>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasStringVector.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/header/IDasStringVector.generated.h>

#include <string>
#include <string_view>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

namespace
{
    using DasVariantType = Das::ExportInterface::DasVariantType;

    using Das::ExportInterface::DAS_VARIANT_TYPE_BASE;
    using Das::ExportInterface::DAS_VARIANT_TYPE_BOOL;
    using Das::ExportInterface::DAS_VARIANT_TYPE_COMPONENT;
    using Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT;
    using Das::ExportInterface::DAS_VARIANT_TYPE_IMAGE;
    using Das::ExportInterface::DAS_VARIANT_TYPE_INT;
    using Das::ExportInterface::DAS_VARIANT_TYPE_JSON;
    using Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
    using Das::ExportInterface::DAS_VARIANT_TYPE_SIGNAL;
    using Das::ExportInterface::DAS_VARIANT_TYPE_STRING;

    using IDasPortMap = Das::ExportInterface::IDasPortMap;
    using IDasReadOnlyPortMap = Das::ExportInterface::IDasReadOnlyPortMap;

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /// Extract std::string from an IDasReadOnlyString*.
    std::string PortIdToString(IDasReadOnlyString* p_str)
    {
        if (p_str == nullptr)
        {
            return {};
        }
        const char* utf8 = nullptr;
        p_str->GetUtf8(&utf8);
        return utf8 ? std::string{utf8} : std::string{};
    }

    /// Extract std::string from a yyjson string value.
    std::string YyjsonAsString(const auto& val)
    {
        auto sv = val.as_string();
        if (sv.has_value())
        {
            return std::string{*sv};
        }
        return {};
    }

} // namespace

// ===========================================================================
// ConvertJsonToPortMap
// ===========================================================================

DasResult ConvertJsonToPortMap(
    const std::string& input_json,
    IDasPortMap*       p_portmap,
    std::string*       p_out_error_message)
{
    if (p_portmap == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // Parse JSON.
    auto parsed = Das::Utils::ParseYyjsonFromString(input_json);
    if (!parsed.has_value())
    {
        if (p_out_error_message != nullptr)
        {
            *p_out_error_message = "JSON parse error";
        }
        return DAS_E_INVALID_JSON;
    }

    auto obj = parsed->as_object();
    if (!obj.has_value())
    {
        if (p_out_error_message != nullptr)
        {
            *p_out_error_message = "Input is not a JSON object";
        }
        return DAS_E_INVALID_JSON;
    }

    for (auto member : *obj)
    {
        const auto        key_sv = member.first;
        const auto&       val = member.second;
        const std::string key_str{key_sv};
        DasReadOnlyString port_id{key_str.c_str()};

        if (val.is_sint())
        {
            p_portmap->SetInt(port_id.Get(), val.as_sint().value_or(0));
        }
        else if (val.is_uint())
        {
            // Unsigned integers stored as int64_t.
            p_portmap->SetInt(
                port_id.Get(),
                static_cast<int64_t>(val.as_uint().value_or(0)));
        }
        else if (val.is_real())
        {
            p_portmap->SetFloat(port_id.Get(), val.as_real().value_or(0.0));
        }
        else if (val.is_string())
        {
            auto              str = YyjsonAsString(val);
            DasReadOnlyString val_str{str.c_str()};
            p_portmap->SetString(port_id.Get(), val_str.Get());
        }
        else if (val.is_bool())
        {
            auto b = val.as_bool();
            p_portmap->SetBool(port_id.Get(), b.value_or(false));
        }
        else if (val.is_null())
        {
            // No SetNull API on IDasPortMap — skip null entries.
        }
        else if (val.is_object() || val.is_array())
        {
            // Serialize nested structure back to JSON string and store via
            // SetString (GetJson delegates to GetString in DasPortMapImpl).
            auto cloned = Das::Utils::CloneYyjsonValue(val);
            auto nested = Das::Utils::SerializeYyjsonValue(cloned);
            if (nested.has_value())
            {
                DasReadOnlyString nested_str{nested->c_str()};
                p_portmap->SetString(port_id.Get(), nested_str.Get());
            }
        }
    }

    return DAS_S_OK;
}

// ===========================================================================
// ConvertPortMapToJson
// ===========================================================================

DasResult ConvertPortMapToJson(
    IDasPortMap* p_portmap,
    std::string& out_result_json)
{
    if (p_portmap == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // Get all keys.
    DAS::DasPtr<Das::ExportInterface::IDasStringVector> keys;
    DasResult result = p_portmap->GetKeys(keys.Put());
    if (DAS::IsFailed(result))
    {
        return result;
    }

    uint64_t count = 0;
    result = keys->Size(&count);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    // Build yyjson mutable object.
    auto root = Das::Utils::MakeYyjsonObject();
    auto obj = *root.as_object();

    for (uint64_t i = 0; i < count; ++i)
    {
        IDasReadOnlyString* p_key = nullptr;
        result = keys->At(i, &p_key);
        if (DAS::IsFailed(result) || p_key == nullptr)
        {
            continue;
        }

        const std::string port_id = PortIdToString(p_key);
        p_key->Release();

        const std::string_view port_id_sv{port_id};

        DasVariantType    kind = DAS_VARIANT_TYPE_NULL;
        DasReadOnlyString key_das{port_id.c_str()};
        result = p_portmap->GetType(key_das.Get(), &kind);
        if (DAS::IsFailed(result))
        {
            continue;
        }

        switch (kind)
        {
        case DAS_VARIANT_TYPE_INT:
        {
            int64_t v{};
            p_portmap->GetInt(key_das.Get(), &v);
            obj[port_id_sv] = v;
            break;
        }
        case DAS_VARIANT_TYPE_FLOAT:
        {
            double v{};
            p_portmap->GetFloat(key_das.Get(), &v);
            obj[port_id_sv] = v;
            break;
        }
        case DAS_VARIANT_TYPE_BOOL:
        {
            bool v{};
            p_portmap->GetBool(key_das.Get(), &v);
            obj[port_id_sv] = v;
            break;
        }
        case DAS_VARIANT_TYPE_STRING:
        case DAS_VARIANT_TYPE_JSON:
        {
            // Both STRING and JSON types store string data.
            // Try to parse as JSON first; if it's a valid object/array,
            // inline it. Otherwise store as a plain string.
            IDasReadOnlyString* p_str = nullptr;
            if (kind == DAS_VARIANT_TYPE_JSON)
            {
                p_portmap->GetJson(key_das.Get(), &p_str);
            }
            else
            {
                p_portmap->GetString(key_das.Get(), &p_str);
            }
            if (p_str != nullptr)
            {
                const char* utf8 = nullptr;
                p_str->GetUtf8(&utf8);
                std::string str_val(utf8 ? utf8 : "");
                p_str->Release();

                // Try to parse as JSON for inlining.
                auto parsed = Das::Utils::ParseYyjsonFromString(str_val);
                if (parsed.has_value()
                    && (parsed->is_object() || parsed->is_array()))
                {
                    // Inline as nested JSON.
                    auto inlined = Das::Utils::CloneYyjsonValue(*parsed);
                    obj[port_id_sv] = std::move(inlined);
                }
                else
                {
                    // Store as plain string.
                    obj[port_id_sv] = std::make_pair(
                        std::string_view(str_val),
                        yyjson::copy_string);
                }
            }
            break;
        }
        case DAS_VARIANT_TYPE_IMAGE:
        {
            // IMAGE entries produce a __das_image_ref__ placeholder.
            auto img_ref = Das::Utils::MakeYyjsonObject();
            auto img_obj = *img_ref.as_object();
            img_obj[std::string_view("__type__")] = std::make_pair(
                std::string_view("image_ref"),
                yyjson::copy_string);
            img_obj[std::string_view("__note__")] = std::make_pair(
                std::string_view("pixel data not serialized to JSON"),
                yyjson::copy_string);

            // Wrap in an outer object: {"__das_image_ref__": {…}}
            auto wrapper = Das::Utils::MakeYyjsonObject();
            auto wrapper_obj = *wrapper.as_object();
            wrapper_obj[std::string_view("__das_image_ref__")] =
                Das::Utils::CloneYyjsonValue(img_ref);

            obj[port_id_sv] = std::move(wrapper);
            break;
        }
        case DAS_VARIANT_TYPE_NULL:
        {
            obj[port_id_sv] = yyjson::value{}; // null
            break;
        }
        default:
        {
            // Unsupported types (BASE, COMPONENT, SIGNAL) — skip.
            break;
        }
        }
    }

    auto serialized = Das::Utils::SerializeYyjsonValue(root);
    if (serialized.has_value())
    {
        out_result_json = std::move(*serialized);
    }
    else
    {
        out_result_json = "{}";
    }
    return DAS_S_OK;
}

// ===========================================================================
// MapDasResultToStatus
// ===========================================================================

const char* MapDasResultToStatus(DasResult result)
{
    if (DAS::IsOk(result))
    {
        return "ok";
    }
    if (result == DAS_E_TIMEOUT)
    {
        return "cancelled";
    }
    return "error";
}

// ===========================================================================
// LegacyJsonTaskComponentAdapter
// ===========================================================================

LegacyJsonTaskComponentAdapter::LegacyJsonTaskComponentAdapter(
    LegacyDoFn legacy_do)
    : legacy_do_(std::move(legacy_do))
{
}

LegacyJsonTaskComponentAdapter::~LegacyJsonTaskComponentAdapter() = default;

DasResult LegacyJsonTaskComponentAdapter::Do(
    Das::PluginInterface::IDasStopToken* /*p_stop_token*/,
    IDasReadOnlyPortMap* p_input,
    IDasPortMap**        pp_out_portmap)
{
    if (pp_out_portmap == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // Step 1: Convert input PortMap to JSON string.
    // Since ConvertPortMapToJson takes IDasPortMap* (writable), but we have
    // IDasReadOnlyPortMap*, we need to copy entries to a writable map first.
    DAS::DasPtr<IDasPortMap> temp_input;
    DasResult                result = CreateIDasPortMap(temp_input.Put());
    if (DAS::IsFailed(result))
    {
        return result;
    }

    // Copy read-only entries to writable map.
    DAS::DasPtr<Das::ExportInterface::IDasStringVector> input_keys;
    result = p_input->GetKeys(input_keys.Put());
    if (DAS::IsFailed(result))
    {
        return result;
    }

    uint64_t input_count = 0;
    result = input_keys->Size(&input_count);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    for (uint64_t i = 0; i < input_count; ++i)
    {
        IDasReadOnlyString* p_key = nullptr;
        result = input_keys->At(i, &p_key);
        if (DAS::IsFailed(result) || p_key == nullptr)
        {
            continue;
        }

        DasVariantType kind = DAS_VARIANT_TYPE_NULL;
        p_input->GetType(p_key, &kind);

        switch (kind)
        {
        case DAS_VARIANT_TYPE_INT:
        {
            int64_t v{};
            p_input->GetInt(p_key, &v);
            temp_input->SetInt(p_key, v);
            break;
        }
        case DAS_VARIANT_TYPE_FLOAT:
        {
            double v{};
            p_input->GetFloat(p_key, &v);
            temp_input->SetFloat(p_key, v);
            break;
        }
        case DAS_VARIANT_TYPE_BOOL:
        {
            bool v{};
            p_input->GetBool(p_key, &v);
            temp_input->SetBool(p_key, v);
            break;
        }
        case DAS_VARIANT_TYPE_STRING:
        {
            IDasReadOnlyString* p_str = nullptr;
            p_input->GetString(p_key, &p_str);
            temp_input->SetString(p_key, p_str);
            if (p_str != nullptr)
            {
                p_str->Release();
            }
            break;
        }
        case DAS_VARIANT_TYPE_JSON:
        {
            IDasReadOnlyString* p_json = nullptr;
            p_input->GetJson(p_key, &p_json);
            // Store JSON as string (GetJson = GetString in impl).
            temp_input->SetString(p_key, p_json);
            if (p_json != nullptr)
            {
                p_json->Release();
            }
            break;
        }
        case DAS_VARIANT_TYPE_NULL:
        {
            // No SetNull — skip.
            break;
        }
        default:
        {
            // BASE, COMPONENT, IMAGE, SIGNAL — skip.
            break;
        }
        }

        p_key->Release();
    }

    std::string input_json_str;
    result = ConvertPortMapToJson(temp_input.Get(), input_json_str);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    // Step 2: Call legacy Do callback.
    std::string result_json_str;
    DasResult   legacy_result = legacy_do_(input_json_str, result_json_str);

    // Step 3: Build output PortMap from result JSON, bound to out-param.
    DAS::DasOutPtr<IDasPortMap> output_map(pp_out_portmap);
    result = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(result))
    {
        return result;
    }

    if (!result_json_str.empty())
    {
        ConvertJsonToPortMap(result_json_str, output_map.Get());
    }

    // Step 4: Inject __status__ into output.
    {
        DasReadOnlyString status_key{"__status__"};
        const char*       status = MapDasResultToStatus(legacy_result);
        DasReadOnlyString status_val{status};
        output_map->SetString(status_key.Get(), status_val.Get());
    }

    // Transfer ownership to caller.
    output_map.Keep();
    return DAS_S_OK;
}

DAS_CORE_GRAPHRUNTIME_NS_END
