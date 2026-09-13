#include <das/Core/GraphRuntime/DoAdapter.h>

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

    using Das::ExportInterface::IDasImage;
    using IDasPortMap = Das::ExportInterface::IDasPortMap;

    DasResult PortMapEntryToPortValue(
        IDasPortMap*       map,
        const std::string& port_id,
        const std::string& node_id,
        PortFrame&         frame)
    {
        const DasReadOnlyString key_str{port_id.c_str()};

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
            DAS::DasPtr<IDasBase> base_ptr;
            result = map->GetBase(key_str.Get(), DAS_IID_BASE, base_ptr.Put());
            if (DAS::IsOk(result))
            {
                frame.Set(
                    node_id,
                    port_id,
                    PortValue(BaseHandle{std::move(base_ptr)}));
            }
            return result;
        }
        case DAS_VARIANT_TYPE_COMPONENT:
        {
            DAS::DasPtr<IDasBase> comp_ptr;
            result =
                map->GetComponent(key_str.Get(), DAS_IID_BASE, comp_ptr.Put());
            if (DAS::IsOk(result))
            {
                frame.Set(
                    node_id,
                    port_id,
                    PortValue(ComponentHandle{std::move(comp_ptr)}));
            }
            return result;
        }
        case DAS_VARIANT_TYPE_IMAGE:
        {
            // GetImage 返回已 AddRef 的拥有引用——一律 Put() 接管，引用随
            // 帧槽位存活（零拷贝）。
            DAS::DasPtr<IDasImage> image_ptr;
            result = map->GetImage(key_str.Get(), image_ptr.Put());
            if (DAS::IsOk(result))
            {
                frame.Set(
                    node_id,
                    port_id,
                    PortValue(ImageData{std::move(image_ptr)}));
            }
            return result;
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
    // each entry into a PortValue::Signal() in the frame so the runtime can
    // gate on it — the component itself never sees signal values
    // (zero-awareness). (DAS-60 Stage 3: signal emission → PortFrame
    // materialisation.)
    constexpr std::string_view EMITTED_SIGNALS_KEY = "signals";

    /// Read the reserved @p "signals" array from @p map and write a
    /// PortValue::Signal() into @p frame for every listed port, under
    /// (@p node_id, signal_port_id). Returns DAS_S_OK even when the key is
    /// absent (no signals emitted is the common, non-control-flow case).
    DasResult ExtractEmittedSignals(
        Das::ExportInterface::IDasPortMap* map,
        const std::string&                 node_id,
        PortFrame&                         frame)
    {
        const DasReadOnlyString key{std::string{EMITTED_SIGNALS_KEY}.c_str()};
        IDasReadOnlyString*     p_str = nullptr;
        const DasResult         result = map->GetString(key.Get(), &p_str);
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

    // Sentinel source marking a broadcast (graph-level input) binding — not
    // a real node, so there is no upstream PortFrame entry to read. Must
    // match GraphCompiler::GRAPH_INPUT_BROADCAST_SOURCE and
    // GraphRuntime::BROADCAST_SOURCE. The binding targets are the derived
    // dangling input ports (the graph's boundary inputs).
    constexpr std::string_view BROADCAST_SOURCE = "$graph_input";

    /// Materialise a broadcast (graph-level input) binding's @p default_value
    /// directly into @p map under @p port_id. Unlike a point-to-point data edge
    /// there is no upstream node to read from the frame; the value comes from
    /// the port's declared default. Scalars (bool/int/float/string) are
    /// set by JSON value kind; null leaves the port unset (the component keeps
    /// its own default); other kinds (object/array) are skipped with a warning
    /// — broadcast inputs are scalar options, mirroring MAA global_option.
    DasResult SetDefaultFromYyjson(
        IDasPortMap*         map,
        const std::string&   port_id,
        const yyjson::value& default_value)
    {
        const DasReadOnlyString key{port_id.c_str()};

        if (default_value.is_null())
        {
            return DAS_S_OK;
        }
        if (auto v = default_value.as_bool())
        {
            return map->SetBool(key.Get(), *v);
        }
        if (auto v = default_value.as_int())
        {
            return map->SetInt(key.Get(), *v);
        }
        if (auto v = default_value.as_real())
        {
            return map->SetFloat(key.Get(), *v);
        }
        if (auto v = default_value.as_string())
        {
            const std::string       s(*v);
            const DasReadOnlyString val{s.c_str()};
            return map->SetString(key.Get(), val.Get());
        }

        DAS_CORE_LOG_WARN(
            "Graph-input default for port_id = {} is a non-scalar JSON value; "
            "skipping broadcast injection.",
            port_id);
        return DAS_S_OK;
    }

    /// Whether @p graph_inputs holds a typed entry under @p port_id.
    bool HasTypedPort(
        Das::ExportInterface::IDasReadOnlyPortMap* graph_inputs,
        const std::string&                         port_id)
    {
        if (graph_inputs == nullptr)
        {
            return false;
        }
        const DasReadOnlyString key{port_id.c_str()};
        bool                    has = false;
        if (DAS::IsFailed(graph_inputs->Has(key.Get(), &has)))
        {
            return false;
        }
        return has;
    }

    /// Transfer the typed graph-level input stored under @p port_id from
    /// @p graph_inputs into @p map under the same key, dispatching on the
    /// runtime variant kind. Object references ride their AddRef/Put()
    /// pairing end to end; json/signal kinds are not call-site inputs and
    /// are skipped silently.
    DasResult TransferTypedPortValue(
        Das::ExportInterface::IDasReadOnlyPortMap* graph_inputs,
        IDasPortMap*                               map,
        const std::string&                         port_id)
    {
        const DasReadOnlyString key{port_id.c_str()};

        DasVariantType kind = DAS_VARIANT_TYPE_NULL;
        DasResult      result = graph_inputs->GetType(key.Get(), &kind);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        switch (kind)
        {
        case DAS_VARIANT_TYPE_INT:
        {
            int64_t value = 0;
            result = graph_inputs->GetInt(key.Get(), &value);
            if (DAS::IsOk(result))
            {
                result = map->SetInt(key.Get(), value);
            }
            return result;
        }
        case DAS_VARIANT_TYPE_FLOAT:
        {
            double value = 0.0;
            result = graph_inputs->GetFloat(key.Get(), &value);
            if (DAS::IsOk(result))
            {
                result = map->SetFloat(key.Get(), value);
            }
            return result;
        }
        case DAS_VARIANT_TYPE_BOOL:
        {
            bool value = false;
            result = graph_inputs->GetBool(key.Get(), &value);
            if (DAS::IsOk(result))
            {
                result = map->SetBool(key.Get(), value);
            }
            return result;
        }
        case DAS_VARIANT_TYPE_STRING:
        {
            IDasReadOnlyString* value_string = nullptr;
            result = graph_inputs->GetString(key.Get(), &value_string);
            if (DAS::IsOk(result) && value_string != nullptr)
            {
                result = map->SetString(key.Get(), value_string);
                value_string->Release();
            }
            return result;
        }
        case DAS_VARIANT_TYPE_IMAGE:
        {
            // GetImage 返回已 AddRef 的拥有引用——Put() 接管，SetImage
            // 自持引用后本地引用析构释放，恰好一次配对。
            DAS::DasPtr<IDasImage> image_ptr;
            result = graph_inputs->GetImage(key.Get(), image_ptr.Put());
            if (DAS::IsOk(result))
            {
                result = map->SetImage(key.Get(), image_ptr.Get());
            }
            return result;
        }
        case DAS_VARIANT_TYPE_BASE:
        {
            DAS::DasPtr<IDasBase> base_ptr;
            result =
                graph_inputs->GetBase(key.Get(), DAS_IID_BASE, base_ptr.Put());
            if (DAS::IsOk(result))
            {
                result = map->SetBase(key.Get(), DAS_IID_BASE, base_ptr.Get());
            }
            return result;
        }
        case DAS_VARIANT_TYPE_COMPONENT:
        {
            DAS::DasPtr<IDasBase> component_ptr;
            result = graph_inputs->GetComponent(
                key.Get(),
                DAS_IID_BASE,
                component_ptr.Put());
            if (DAS::IsOk(result))
            {
                result = map->SetComponent(
                    key.Get(),
                    DAS_IID_BASE,
                    component_ptr.Get());
            }
            return result;
        }
        case DAS_VARIANT_TYPE_NULL:
        case DAS_VARIANT_TYPE_JSON:
        case DAS_VARIANT_TYPE_SIGNAL:
        {
            DAS_CORE_LOG_TRACE(
                "Graph-input pass-through skipped non-transferable kind = {} "
                "for port_id = {}.",
                static_cast<int>(kind),
                port_id);
            return DAS_S_OK;
        }
        default:
        {
            DAS_CORE_LOG_WARN(
                "Graph-input pass-through saw unknown kind = {} for port_id "
                "= {}.",
                static_cast<int>(kind),
                port_id);
            return DAS_S_OK;
        }
        }
    }

    // 数字等价族判定与 GraphCompiler::IsTypeCompatible 的整数/浮点族划
    // 分同源（同一张等价表）——读值侧物化只关心目标口属于哪个族。
    bool IsIntegerFamilyPortType(const std::string& port_type)
    {
        return port_type == "int" || port_type == "int64";
    }

    bool IsFloatFamilyPortType(const std::string& port_type)
    {
        return port_type == "float" || port_type == "double";
    }

} // namespace

// ===========================================================================
// PortValueToPortMap
// ===========================================================================

DasResult PortValueToPortMap(
    const PortValue&                   pv,
    const std::string&                 port_id,
    Das::ExportInterface::IDasPortMap* map)
{
    // Create a DasReadOnlyString for the port_id key.
    const DasReadOnlyString key_str{port_id.c_str()};

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
        const auto&             s = *pv.AsString();
        const DasReadOnlyString val_str{s.c_str()};
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
        // SetImage 内部自持引用——传非拥有裸指针；空引用对齐 Base 分支
        // 静默跳过，组件见缺键回落自身默认。
        auto* raw = pv.AsImage()->ptr.Get();
        if (raw != nullptr)
        {
            return map->SetImage(key_str.Get(), raw);
        }
        return DAS_S_OK;
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

// ===========================================================================
// BuildInputPortMap
// ===========================================================================

DasResult BuildInputPortMap(
    const PortFrame&                           frame,
    const std::vector<Dto::PortBindingDto>&    bindings,
    Das::ExportInterface::IDasPortMap**        out_map,
    Das::ExportInterface::IDasReadOnlyPortMap* graph_inputs)
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
        // Broadcast (graph-level input) binding: the typed call-site value
        // (graph_inputs) wins over the declared default — the sentinel
        // source marks no real upstream node, so there is no frame entry to
        // read either way.
        if (binding.source_node_id == std::string{BROADCAST_SOURCE})
        {
            if (HasTypedPort(graph_inputs, binding.target_port_id))
            {
                const DasResult transfer_result = TransferTypedPortValue(
                    graph_inputs,
                    map.Get(),
                    binding.target_port_id);
                if (DAS::IsFailed(transfer_result))
                {
                    DAS_CORE_LOG_ERROR(
                        "Failed to transfer typed graph-level input for "
                        "target_port_id = {}.",
                        binding.target_port_id);
                    return transfer_result;
                }
                DAS_CORE_LOG_TRACE(
                    "Graph-level input for target_port_id = {} taken from "
                    "typed call-site value (default not materialised).",
                    binding.target_port_id);
                continue;
            }
            const DasResult r = SetDefaultFromYyjson(
                map.Get(),
                binding.target_port_id,
                binding.default_value);
            if (DAS::IsFailed(r))
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to materialise graph-level input default for "
                    "target_port_id = {}.",
                    binding.target_port_id);
                return r;
            }
            continue;
        }

        // Look up the source value in the frame by string node key.
        const PortKey source_key{
            binding.source_node_id,
            binding.source_port_id};
        const auto* pv = frame.Find(source_key);
        if (pv == nullptr)
        {
            DAS_CORE_LOG_WARN(
                "Source port not found: node = {}, port = {}.",
                binding.source_node_id,
                binding.source_port_id);
            continue;
        }

        // 读值侧物化：生产者按自身声明类型写帧，消费侧在装配点把帧值
        // 物化成目标口的声明类型（binding.target_type）。整数→浮点是
        // 无损宽化；浮点→整数是精度截断，从不静默做——WARN 后跳过该
        // 键，组件见缺键回落自身默认（与「源口无帧值」的缺口语径同
        // 构）。合法 plan 已在编译期被方向规则拒绝，此处是运行期防
        // 御位，不该再有截断副作用。
        if (IsFloatFamilyPortType(binding.target_type) && pv->IsInt())
        {
            const DasReadOnlyString key{binding.target_port_id.c_str()};
            result = map.Get()->SetFloat(
                key.Get(),
                static_cast<double>(*pv->AsInt()));
            if (DAS::IsFailed(result))
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to materialise widened value for "
                    "target_port_id = {}.",
                    binding.target_port_id);
                return result;
            }
            continue;
        }
        if (IsIntegerFamilyPortType(binding.target_type) && pv->IsFloat())
        {
            DAS_CORE_LOG_WARN(
                "Float-to-integer materialisation refused: source = {}.{}, "
                "target = {}.{}, target_type = {}. The target key stays "
                "absent; the component falls back to its own default.",
                binding.source_node_id,
                binding.source_port_id,
                binding.target_node_id,
                binding.target_port_id,
                binding.target_type);
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
    const std::string&                 node_id,
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
        if (port_id == std::string{EMITTED_SIGNALS_KEY})
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
