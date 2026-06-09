#include <das/Core/GraphRuntime/DoAdapter.h>

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/Logger/Logger.h>
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
            return DAS_E_NOT_SUPPORTED;
        }
        if (pv.IsJson())
        {
            // JSON serialised as string — GetJson returns IDasReadOnlyString
            // in the PortMap contract.
            DAS_CORE_LOG_WARN(
                "Json type not yet supported in DoAdapter for port_id = {}.",
                port_id);
            return DAS_E_NOT_SUPPORTED;
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

    // Create a fresh IDasPortMap via the C API factory.
    DAS::DasPtr<Das::ExportInterface::IDasPortMap> map;
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

    *out_map = map.Detach();
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
