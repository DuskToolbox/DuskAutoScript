#include <das/Core/GraphRuntime/DoAdapter.h>

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasStringVector.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/header/IDasStringVector.generated.h>

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

    DasResult PortValueToPortMap(
        const PortValue&   pv,
        const std::string& port_id,
        IDasPortMap*       map)
    {
        // Create a DasReadOnlyString for the port_id key.
        DasReadOnlyString key_str{port_id.c_str()};

        if (pv.IsNull())
        {
            return DAS_S_OK;
        }
        if (pv.IsInt())
        {
            return map->SetInt(key_str.Get(), *pv.AsInt());
        }
        if (pv.IsFloat())
        {
            return map->SetFloat(key_str.Get(), *pv.AsFloat());
        }
        if (pv.IsString())
        {
            const auto&       s = *pv.AsString();
            DasReadOnlyString val_str{s.c_str()};
            return map->SetString(key_str.Get(), val_str.Get());
        }
        if (pv.IsBool())
        {
            return map->SetBool(key_str.Get(), *pv.AsBool());
        }
        if (pv.IsBase())
        {
            auto* raw = pv.AsBase()->ptr.Get();
            if (raw != nullptr)
            {
                return map->SetBase(key_str.Get(), DAS_IID_BASE, raw);
            }
            return DAS_S_OK;
        }
        if (pv.IsComponent())
        {
            auto* raw = pv.AsComponent()->ptr.Get();
            if (raw != nullptr)
            {
                return map->SetComponent(key_str.Get(), DAS_IID_BASE, raw);
            }
            return DAS_S_OK;
        }
        if (pv.IsImage())
        {
            DAS_CORE_LOG_WARN(
                "Image type not yet supported in DoAdapter for port_id = {}.",
                port_id);
            return DAS_E_NO_IMPLEMENTATION;
        }
        if (pv.IsJson())
        {
            // JSON serialised as string — GetJson returns IDasReadOnlyString
            // in the PortMap contract.
            DAS_CORE_LOG_WARN(
                "Json type not yet supported in DoAdapter for port_id = {}.",
                port_id);
            return DAS_E_NO_IMPLEMENTATION;
        }
        if (pv.IsSignal())
        {
            // Signals are control-flow markers — no data payload.
            return DAS_S_OK;
        }

        DAS_CORE_LOG_ERROR(
            "Unhandled PortValue type = {} for port_id = {}. "
            "Skipping binding.",
            static_cast<int>(pv.GetType()),
            port_id);
        return DAS_S_OK;
    }

    DasResult PortMapEntryToPortValue(
        IDasPortMap*       map,
        const std::string& port_id,
        DasGuid            node_id,
        PortFrame&         frame)
    {
        DasReadOnlyString key_str{port_id.c_str()};

        DasVariantType kind = DAS_VARIANT_TYPE_NULL;
        DasResult      result = map->GetType(key_str.Get(), &kind);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        switch (kind)
        {
        case DAS_VARIANT_TYPE_INT:
        {
            int64_t val{};
            result = map->GetInt(key_str.Get(), &val);
            if (DAS::IsOk(result))
            {
                frame.Set(node_id, port_id, PortValue(val));
            }
            return result;
        }
        case DAS_VARIANT_TYPE_FLOAT:
        {
            double val{};
            result = map->GetFloat(key_str.Get(), &val);
            if (DAS::IsOk(result))
            {
                frame.Set(node_id, port_id, PortValue(val));
            }
            return result;
        }
        case DAS_VARIANT_TYPE_STRING:
        {
            IDasReadOnlyString* p_str = nullptr;
            result = map->GetString(key_str.Get(), &p_str);
            if (DAS::IsOk(result) && p_str != nullptr)
            {
                const char* utf8 = nullptr;
                p_str->GetUtf8(&utf8);
                frame.Set(
                    node_id,
                    port_id,
                    PortValue(std::string(utf8 ? utf8 : "")));
                p_str->Release();
            }
            return result;
        }
        case DAS_VARIANT_TYPE_BOOL:
        {
            bool val{};
            result = map->GetBool(key_str.Get(), &val);
            if (DAS::IsOk(result))
            {
                frame.Set(node_id, port_id, PortValue(val));
            }
            return result;
        }
        case DAS_VARIANT_TYPE_BASE:
        {
            IDasBase* p_obj = nullptr;
            result = map->GetBase(key_str.Get(), DAS_IID_BASE, &p_obj);
            if (DAS::IsOk(result))
            {
                // Attach adopts the +1 ref from QueryInterface inside
                // GetBase — no extra AddRef.  Using DasPtr(raw) here
                // would double-count and leak (DasPtr.hpp:61 calls
                // AddRef on raw-pointer construction).
                auto base_ptr = DAS::DasPtr<IDasBase>::Attach(p_obj);
                frame.Set(
                    node_id,
                    port_id,
                    PortValue(BaseHandle{std::move(base_ptr)}));
            }
            return result;
        }
        case DAS_VARIANT_TYPE_COMPONENT:
        {
            IDasBase* p_comp = nullptr;
            result = map->GetComponent(key_str.Get(), DAS_IID_BASE, &p_comp);
            if (DAS::IsOk(result))
            {
                // Same pattern: GetComponent returns +1 ref, Attach
                // adopts it without double counting.
                auto comp_ptr = DAS::DasPtr<IDasBase>::Attach(p_comp);
                frame.Set(
                    node_id,
                    port_id,
                    PortValue(ComponentHandle{std::move(comp_ptr)}));
            }
            return result;
        }
        case DAS_VARIANT_TYPE_IMAGE:
        {
            DAS_CORE_LOG_WARN(
                "Image extraction not yet supported for port_id = {}.",
                port_id);
            return DAS_S_OK;
        }
        case DAS_VARIANT_TYPE_NULL:
        {
            // Null entries carry no data — nothing to write back.
            return DAS_S_OK;
        }
        case DAS_VARIANT_TYPE_JSON:
        case DAS_VARIANT_TYPE_SIGNAL:
        {
            DAS_CORE_LOG_WARN(
                "Unsupported variant type = {} for port_id = {}.",
                static_cast<int>(kind),
                port_id);
            return DAS_S_OK;
        }
        default:
        {
            DAS_CORE_LOG_ERROR(
                "Unknown variant type = {} for port_id = {}.",
                static_cast<int>(kind),
                port_id);
            return DAS_E_FAIL;
        }
        }
    }

    // Reserved output key carrying the component's emitted signal list.
    // A component fires its signal output ports by listing their port IDs in a
    // JSON string array under this key (e.g. R"(["true"])"). The adapter turns
    // each entry into a PortValue::Signal() in the frame so the runtime can gate
    // on it — the component itself never sees signal values (zero-awareness).
    // (DAS-60 Stage 3: signal emission → PortFrame materialisation.)
    constexpr std::string_view kEmittedSignalsKey = "signals";

    /// Read the reserved @p "signals" array from @p map and write a
    /// PortValue::Signal() into @p frame for every listed port, under
    /// (@p node_id, signal_port_id). Returns DAS_S_OK even when the key is
    /// absent (no signals emitted is the common, non-control-flow case).
    DasResult ExtractEmittedSignals(
        Das::ExportInterface::IDasPortMap* map,
        DasGuid                            node_id,
        PortFrame&                         frame)
    {
        DasReadOnlyString  key{std::string{kEmittedSignalsKey}.c_str()};
        IDasReadOnlyString* p_str = nullptr;
        DasResult           result = map->GetString(key.Get(), &p_str);
        if (DAS::IsFailed(result) || p_str == nullptr)
        {
            return DAS_S_OK;
        }

        const char* utf8 = nullptr;
        p_str->GetUtf8(&utf8);
        std::string json_text(utf8 ? utf8 : "");
        p_str->Release();

        if (json_text.empty())
        {
            return DAS_S_OK;
        }

        auto parsed = Das::Utils::ParseYyjsonFromString(json_text);
        if (!parsed.has_value())
        {
            DAS_CORE_LOG_WARN(
                "Emitted signals key held non-JSON text: '{}'.",
                json_text);
            return DAS_S_OK;
        }

        auto arr = parsed->as_array();
        if (!arr.has_value())
        {
            return DAS_S_OK;
        }

        for (const auto& entry : *arr)
        {
            auto name = entry.as_string();
            if (!name.has_value())
            {
                continue;
            }
            frame.Set(node_id, std::string(*name), PortValue::Signal());
        }
        return DAS_S_OK;
    }

} // namespace

// ===========================================================================
// BuildInputPortMap
// ===========================================================================

DasResult BuildInputPortMap(
    const PortFrame&                        frame,
    const std::vector<Dto::PortBindingDto>& bindings,
    Das::ExportInterface::IDasPortMap**     out_map)
{
    if (out_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // Create a fresh IDasPortMap via the C API factory, bound to out_map.
    DAS::DasOutPtr<Das::ExportInterface::IDasPortMap> map(out_map);
    DasResult result = CreateIDasPortMap(map.Put());
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("CreateIDasPortMap failed.");
        return result;
    }

    for (const auto& binding : bindings)
    {
        // Convert string node_id → DasGuid and look up the source value.
        const auto source_node = DAS::Core::ForeignInterfaceHost::MakeDasGuid(
            binding.source_node_id);

        const PortKey source_key{source_node, binding.source_port_id};
        const auto*   pv = frame.Find(source_key);
        if (pv == nullptr)
        {
            DAS_CORE_LOG_WARN(
                "Source port not found: node = {}, port = {}.",
                binding.source_node_id,
                binding.source_port_id);
            continue;
        }

        result = PortValueToPortMap(*pv, binding.target_port_id, map.Get());
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_ERROR(
                "Failed to set port value for target_port_id = {}.",
                binding.target_port_id);
            return result;
        }
    }

    map.Keep();
    return DAS_S_OK;
}

// ===========================================================================
// ExtractOutputPortMap
// ===========================================================================

DasResult ExtractOutputPortMap(
    Das::ExportInterface::IDasPortMap* output_map,
    DasGuid                            node_id,
    PortFrame&                         frame)
{
    if (output_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // Retrieve all keys from the output map.
    DAS::DasPtr<Das::ExportInterface::IDasStringVector> keys;
    DasResult result = output_map->GetKeys(keys.Put());
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("GetKeys failed on output IDasPortMap.");
        return result;
    }

    uint64_t count = 0;
    // IDasStringVector::Size returns DasResult in low bits, size in high.
    result = keys->Size(&count);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    for (uint64_t i = 0; i < count; ++i)
    {
        IDasReadOnlyString* p_key = nullptr;
        result = keys->At(i, &p_key);
        if (DAS::IsFailed(result) || p_key == nullptr)
        {
            DAS_CORE_LOG_WARN("Failed to read key at index = {}.", i);
            continue;
        }

        const char* utf8 = nullptr;
        p_key->GetUtf8(&utf8);
        std::string port_id(utf8 ? utf8 : "");

        // The reserved "signals" key is control-flow metadata, not a data
        // port — materialise it as Signal markers and skip normal extraction.
        if (port_id == std::string{kEmittedSignalsKey})
        {
            ExtractEmittedSignals(output_map, node_id, frame);
            p_key->Release();
            continue;
        }

        result = PortMapEntryToPortValue(output_map, port_id, node_id, frame);
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_WARN(
                "Failed to extract output port: port_id = {}.",
                port_id);
        }

        p_key->Release();
    }

    return DAS_S_OK;
}

DAS_CORE_GRAPHRUNTIME_NS_END
