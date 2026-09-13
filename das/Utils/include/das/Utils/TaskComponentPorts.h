#ifndef DAS_UTILS_TASKCOMPONENTPORTS_H
#define DAS_UTILS_TASKCOMPONENTPORTS_H

#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/DasJson.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasStringVector.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/header/IDasStringVector.generated.h>

#include <optional>
#include <string>
#include <string_view>

/// 任务组件（IDasTaskComponent 实现）与宿主之间经 IDasPortMap 交换数据的
/// 公共读写与桥接 helper。全部为 header-only inline 实现，只消费冻结的
/// IDasPortMap / IDasJson ABI 面，不含任何插件业务语义。
namespace Das::Utils
{
    /// Deep-copy a yyjson value via a serialize/parse round trip.
    inline yyjson::value CloneJson(const yyjson::value& value)
    {
        const auto serialized = value.write(yyjson::WriteFlag::NoFlag);
        auto       parsed = Das::Utils::ParseYyjsonFromString(
            std::string_view(serialized.data(), serialized.size()));
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    /// Bridge a yyjson value into an IDasJson ABI object (empty DasPtr on
    /// failure).
    inline DasPtr<ExportInterface::IDasJson> WrapJson(yyjson::value value)
    {
        auto serialized = Das::Utils::SerializeYyjsonValue(value);
        if (!serialized)
        {
            return {};
        }

        DasPtr<ExportInterface::IDasJson> result;
        ParseDasJsonFromString(serialized->c_str(), result.Put());
        return result;
    }

    /// Parse an IDasJson ABI object back into a yyjson value (nullopt when
    /// the object is null or its text cannot be parsed).
    inline std::optional<yyjson::value> ReadJson(
        ExportInterface::IDasJson* json)
    {
        if (!json)
        {
            return std::nullopt;
        }
        DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put())) || !text)
        {
            return std::nullopt;
        }
        const char* raw = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&raw)) || raw == nullptr)
        {
            return std::nullopt;
        }
        return Das::Utils::ParseYyjsonFromString(raw);
    }

    /// Build a yyjson string value from a string view.
    inline yyjson::value JsonString(std::string_view value)
    {
        return yyjson::value(std::string(value));
    }

    /// Write a string value into a PortMap port.
    inline void SetPortString(
        ExportInterface::IDasPortMap* map,
        std::string_view              port_id,
        std::string_view              value)
    {
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        const DasReadOnlyString val{std::string{value}.c_str()};
        map->SetString(key.Get(), val.Get());
    }

    /// Write a JSON value (as serialized string) into a PortMap port.
    inline void SetPortJson(
        ExportInterface::IDasPortMap* map,
        std::string_view              port_id,
        const yyjson::value&          value)
    {
        auto serialized = Das::Utils::SerializeYyjsonValue(value);
        if (serialized)
        {
            const DasReadOnlyString key{std::string{port_id}.c_str()};
            const DasReadOnlyString val{serialized->c_str()};
            map->SetString(key.Get(), val.Get());
        }
    }

    /// Read a JSON value from a PortMap string port. Empty port text and
    /// missing ports both yield nullopt; a JSON "null" document yields a
    /// null value.
    inline std::optional<yyjson::value> GetPortJson(
        ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                      port_id)
    {
        IDasReadOnlyString*     p_str = nullptr;
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        auto                    hr = map->GetString(key.Get(), &p_str);
        if (DAS::IsFailed(hr) || p_str == nullptr)
        {
            return std::nullopt;
        }

        const char* utf8 = nullptr;
        p_str->GetUtf8(&utf8);
        const std::string str_val(utf8 ? utf8 : "");
        p_str->Release();

        if (str_val.empty())
        {
            return std::nullopt;
        }

        return Das::Utils::ParseYyjsonFromString(str_val);
    }

    /// Read a bool from a PortMap port (default on missing or type
    /// mismatch).
    inline bool GetPortBool(
        ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                      port_id,
        bool                                  default_val = false)
    {
        bool                    val = default_val;
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        auto                    hr = map->GetBool(key.Get(), &val);
        return DAS::IsOk(hr) ? val : default_val;
    }

    /// Read an int from a PortMap port (default on missing or type
    /// mismatch).
    inline int64_t GetPortInt(
        ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                      port_id,
        int64_t                               default_val = 0)
    {
        int64_t                 val = default_val;
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        auto                    hr = map->GetInt(key.Get(), &val);
        return DAS::IsOk(hr) ? val : default_val;
    }

    /// Write an int value into a PortMap port.
    inline void SetPortInt(
        ExportInterface::IDasPortMap* map,
        std::string_view              port_id,
        int64_t                       value)
    {
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        map->SetInt(key.Get(), value);
    }

    /// Read a float from a PortMap port (default on missing or type
    /// mismatch). The float family rides the double ABI slot end to end.
    inline double GetPortFloat(
        ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                      port_id,
        double                                default_val = 0.0)
    {
        double                  val = default_val;
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        auto                    hr = map->GetFloat(key.Get(), &val);
        return DAS::IsOk(hr) ? val : default_val;
    }

    /// Write a float value into a PortMap port.
    inline void SetPortFloat(
        ExportInterface::IDasPortMap* map,
        std::string_view              port_id,
        double                        value)
    {
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        map->SetFloat(key.Get(), value);
    }

    /// Emit a single control-flow signal by listing its port id in the
    /// reserved "signals" JSON string array. The runtime materialises each
    /// entry as a PortValue::Signal() under (node_id, signal_port_id) and
    /// gates on it; the component itself never reads signal values. The array
    /// is assembled via the yyjson writer so port ids needing JSON escaping
    /// survive the round trip.
    inline void EmitSignal(
        ExportInterface::IDasPortMap* map,
        std::string_view              signal_port_id)
    {
        auto signals_array = Das::Utils::MakeYyjsonArray();
        auto array_ref = signals_array.as_array();
        if (array_ref)
        {
            array_ref->emplace_back(yyjson::value(std::string(signal_port_id)));
        }
        auto serialized = Das::Utils::SerializeYyjsonValue(signals_array);
        if (serialized.has_value())
        {
            SetPortString(map, "signals", *serialized);
        }
    }

    /// Copy a typed value from @p src_port of @p src into @p dst_port of
    /// @p dst, preserving its runtime variant kind. Returns false when the
    /// source port is absent or null (used by merge to detect which branch
    /// produced a value). Covers the scalar kinds the data plane carries
    /// end-to-end; object/image kinds are not pass-through-able here.
    inline bool CopyTypedPort(
        ExportInterface::IDasReadOnlyPortMap* src,
        std::string_view                      src_port,
        ExportInterface::IDasPortMap*         dst,
        std::string_view                      dst_port)
    {
        using DasVariantType = Das::ExportInterface::DasVariantType;
        using Das::ExportInterface::DAS_VARIANT_TYPE_BOOL;
        using Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT;
        using Das::ExportInterface::DAS_VARIANT_TYPE_INT;
        using Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
        using Das::ExportInterface::DAS_VARIANT_TYPE_STRING;

        const DasReadOnlyString key{std::string{src_port}.c_str()};
        DasVariantType          kind = DAS_VARIANT_TYPE_NULL;
        if (DAS::IsFailed(src->GetType(key.Get(), &kind)))
        {
            return false;
        }
        switch (kind)
        {
        case DAS_VARIANT_TYPE_INT:
        {
            int64_t v{};
            if (DAS::IsOk(src->GetInt(key.Get(), &v)))
            {
                SetPortInt(dst, dst_port, v);
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_BOOL:
        {
            bool v{};
            if (DAS::IsOk(src->GetBool(key.Get(), &v)))
            {
                const DasReadOnlyString dkey{std::string{dst_port}.c_str()};
                dst->SetBool(dkey.Get(), v);
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_FLOAT:
        {
            double v{};
            if (DAS::IsOk(src->GetFloat(key.Get(), &v)))
            {
                const DasReadOnlyString dkey{std::string{dst_port}.c_str()};
                dst->SetFloat(dkey.Get(), v);
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_STRING:
        {
            IDasReadOnlyString* p_str = nullptr;
            if (DAS::IsOk(src->GetString(key.Get(), &p_str))
                && p_str != nullptr)
            {
                const DasReadOnlyString dkey{std::string{dst_port}.c_str()};
                dst->SetString(dkey.Get(), p_str);
                p_str->Release();
                return true;
            }
            return false;
        }
        default:
            return false;
        }
    }

    /// Copy a typed value from @p src_port of @p src into @p dst_port of
    /// @p dst, preserving the runtime variant kind across every
    /// pass-through-able kind — including object references (image/base/
    /// component), which the JSON bridge can never carry. Returns false
    /// when the source port is absent or holds a non-transferable kind
    /// (json/signal). Object references ride their Get/Put() AddRef
    /// pairing exactly once.
    inline bool CopyPortMapEntry(
        ExportInterface::IDasReadOnlyPortMap* src,
        std::string_view                      src_port,
        ExportInterface::IDasPortMap*         dst,
        std::string_view                      dst_port)
    {
        using DasVariantType = Das::ExportInterface::DasVariantType;
        using Das::ExportInterface::DAS_VARIANT_TYPE_BASE;
        using Das::ExportInterface::DAS_VARIANT_TYPE_BOOL;
        using Das::ExportInterface::DAS_VARIANT_TYPE_COMPONENT;
        using Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT;
        using Das::ExportInterface::DAS_VARIANT_TYPE_IMAGE;
        using Das::ExportInterface::DAS_VARIANT_TYPE_INT;
        using Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
        using Das::ExportInterface::DAS_VARIANT_TYPE_SIGNAL;
        using Das::ExportInterface::DAS_VARIANT_TYPE_STRING;

        const DasReadOnlyString key{std::string{src_port}.c_str()};
        DasVariantType          kind = DAS_VARIANT_TYPE_NULL;
        if (DAS::IsFailed(src->GetType(key.Get(), &kind)))
        {
            return false;
        }
        switch (kind)
        {
        case DAS_VARIANT_TYPE_INT:
        {
            int64_t value{};
            if (DAS::IsOk(src->GetInt(key.Get(), &value)))
            {
                SetPortInt(dst, dst_port, value);
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_BOOL:
        {
            bool value{};
            if (DAS::IsOk(src->GetBool(key.Get(), &value)))
            {
                const DasReadOnlyString dst_key{std::string{dst_port}.c_str()};
                dst->SetBool(dst_key.Get(), value);
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_FLOAT:
        {
            double value{};
            if (DAS::IsOk(src->GetFloat(key.Get(), &value)))
            {
                SetPortFloat(dst, dst_port, value);
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_STRING:
        {
            IDasReadOnlyString* value_string = nullptr;
            if (DAS::IsOk(src->GetString(key.Get(), &value_string))
                && value_string != nullptr)
            {
                const DasReadOnlyString dst_key{std::string{dst_port}.c_str()};
                dst->SetString(dst_key.Get(), value_string);
                value_string->Release();
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_IMAGE:
        {
            // GetImage 返回已 AddRef 的拥有引用——Put() 接管，SetImage
            // 自持引用后本地引用析构释放，恰好一次配对。
            DasPtr<ExportInterface::IDasImage> image_ptr;
            if (DAS::IsOk(src->GetImage(key.Get(), image_ptr.Put())))
            {
                const DasReadOnlyString dst_key{std::string{dst_port}.c_str()};
                dst->SetImage(dst_key.Get(), image_ptr.Get());
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_BASE:
        {
            DasPtr<IDasBase> base_ptr;
            if (DAS::IsOk(
                    src->GetBase(key.Get(), DAS_IID_BASE, base_ptr.Put())))
            {
                const DasReadOnlyString dst_key{std::string{dst_port}.c_str()};
                dst->SetBase(dst_key.Get(), DAS_IID_BASE, base_ptr.Get());
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_COMPONENT:
        {
            DasPtr<IDasBase> component_ptr;
            if (DAS::IsOk(src->GetComponent(
                    key.Get(),
                    DAS_IID_BASE,
                    component_ptr.Put())))
            {
                const DasReadOnlyString dst_key{std::string{dst_port}.c_str()};
                dst->SetComponent(
                    dst_key.Get(),
                    DAS_IID_BASE,
                    component_ptr.Get());
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_NULL:
        case DAS_VARIANT_TYPE_SIGNAL:
        default:
        {
            return false;
        }
        }
    }

    /// Read a raw string from a PortMap string port (no JSON parsing).
    /// Use this for ports written via SetPortString (e.g. "status"), as
    /// opposed to GetPortJson which assumes the port holds JSON text.
    inline std::string GetPortString(
        ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                      port_id,
        std::string_view                      default_val = {})
    {
        IDasReadOnlyString*     p_str = nullptr;
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        auto                    hr = map->GetString(key.Get(), &p_str);
        if (DAS::IsFailed(hr) || p_str == nullptr)
        {
            return std::string{default_val};
        }
        const char* utf8 = nullptr;
        p_str->GetUtf8(&utf8);
        std::string val(utf8 && *utf8 ? utf8 : std::string{default_val});
        p_str->Release();
        return val;
    }

    /// Build a result PortMap from a status string, outputs JSON, and
    /// signals JSON array.
    inline DasResult BuildResultPortMap(
        std::string_view               status,
        const yyjson::value&           outputs,
        const yyjson::value&           signals,
        ExportInterface::IDasPortMap** pp_out_port_map)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        const DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }

        SetPortString(output_map.Get(), "status", status);
        SetPortJson(output_map.Get(), "outputs", outputs);
        SetPortJson(output_map.Get(), "signals", signals);

        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }
} // namespace Das::Utils

// 与其余 das/Utils 头保持一致：Core 侧命名空间内以 Utils:: 前缀可达。
namespace Das::Core::Utils
{
}

#endif // DAS_UTILS_TASKCOMPONENTPORTS_H
