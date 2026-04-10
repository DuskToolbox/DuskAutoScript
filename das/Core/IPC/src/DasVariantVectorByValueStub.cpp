#include <das/Core/IPC/DasVariantVectorByValueStub.h>

#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/InterfaceParamSerialization.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MemorySerializer.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasGuidHolder.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/DasTypes.hpp>
#include <das/_autogen/idl/ipc/IpcProxyFactory.h>

#include <cstring>

DAS_CORE_IPC_NS_BEGIN

// ============================================================
// Method ID mapping (0-20):
//   0: GetInt,        1: GetFloat,       2: GetString
//   3: GetBool,       4: GetComponent,   5: GetBase
//   6: SetInt,        7: SetFloat,       8: SetString
//   9: SetBool,      10: SetComponent,  11: SetBase
//  12: PushBackInt,  13: PushBackFloat, 14: PushBackString
//  15: PushBackBool, 16: PushBackComponent, 17: PushBackBase
//  18: GetType,      19: RemoveAt,      20: GetSize
// ============================================================

namespace
{
    /// Read int64_t from params at given offset
    bool ReadInt64(
        const uint8_t* params,
        size_t         params_size,
        size_t&        offset,
        int64_t&       out)
    {
        if (offset + sizeof(int64_t) > params_size)
        {
            return false;
        }
        std::memcpy(&out, params + offset, sizeof(int64_t));
        offset += sizeof(int64_t);
        return true;
    }

    /// Read uint64_t from params at given offset
    bool ReadUInt64(
        const uint8_t* params,
        size_t         params_size,
        size_t&        offset,
        uint64_t&      out)
    {
        if (offset + sizeof(uint64_t) > params_size)
        {
            return false;
        }
        std::memcpy(&out, params + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        return true;
    }

    /// Read uint32_t from params at given offset
    bool ReadUInt32(
        const uint8_t* params,
        size_t         params_size,
        size_t&        offset,
        uint32_t&      out)
    {
        if (offset + sizeof(uint32_t) > params_size)
        {
            return false;
        }
        std::memcpy(&out, params + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        return true;
    }

    /// Read uint16_t from params at given offset
    bool ReadUInt16(
        const uint8_t* params,
        size_t         params_size,
        size_t&        offset,
        uint16_t&      out)
    {
        if (offset + sizeof(uint16_t) > params_size)
        {
            return false;
        }
        std::memcpy(&out, params + offset, sizeof(uint16_t));
        offset += sizeof(uint16_t);
        return true;
    }

    /// Read float from params at given offset
    bool ReadFloat(
        const uint8_t* params,
        size_t         params_size,
        size_t&        offset,
        float&         out)
    {
        if (offset + sizeof(float) > params_size)
        {
            return false;
        }
        std::memcpy(&out, params + offset, sizeof(float));
        offset += sizeof(float);
        return true;
    }

    /// Read bool (1 byte uint8_t) from params at given offset
    bool ReadBool(
        const uint8_t* params,
        size_t         params_size,
        size_t&        offset,
        bool&          out)
    {
        if (offset + sizeof(uint8_t) > params_size)
        {
            return false;
        }
        uint8_t raw = 0;
        std::memcpy(&raw, params + offset, sizeof(uint8_t));
        out = (raw != 0);
        offset += sizeof(uint8_t);
        return true;
    }

    /// Read string ([len:8B][bytes:len]) from params at given offset
    bool ReadString(
        const uint8_t* params,
        size_t         params_size,
        size_t&        offset,
        std::string&   out)
    {
        uint64_t str_len = 0;
        if (!ReadUInt64(params, params_size, offset, str_len))
        {
            return false;
        }
        if (offset + str_len > params_size)
        {
            return false;
        }
        out.assign(
            reinterpret_cast<const char*>(params + offset),
            static_cast<size_t>(str_len));
        offset += static_cast<size_t>(str_len);
        return true;
    }

    /// Write a single variant value to the snapshot writer.
    /// Uses DasVariantType enum values: INT=0, FLOAT=1, STRING=2, BOOL=3,
    /// BASE=4, COMPONENT=5
    DasResult WriteVariantValue(
        MemorySerializerWriter&                  writer,
        Das::ExportInterface::DasVariantType     type,
        Das::ExportInterface::IDasVariantVector* impl,
        uint64_t                                 index,
        StubContext&                             ctx)
    {
        using Das::ExportInterface::DasVariantType;

        writer.WriteUInt32(static_cast<uint32_t>(type));

        switch (type)
        {
        case DasVariantType::DAS_VARIANT_TYPE_INT:
        {
            int64_t   value = 0;
            DasResult result = impl->GetInt(index, &value);
            if (DAS::IsFailed(result))
            {
                return result;
            }
            writer.WriteInt64(value);
            return DAS_S_OK;
        }
        case DasVariantType::DAS_VARIANT_TYPE_FLOAT:
        {
            float     value = 0.0f;
            DasResult result = impl->GetFloat(index, &value);
            if (DAS::IsFailed(result))
            {
                return result;
            }
            writer.WriteFloat(value);
            return DAS_S_OK;
        }
        case DasVariantType::DAS_VARIANT_TYPE_STRING:
        {
            DAS::DasPtr<IDasReadOnlyString> str_obj;
            DasResult result = impl->GetString(index, str_obj.Put());
            if (DAS::IsFailed(result))
            {
                return result;
            }
            const char* utf8_data = nullptr;
            result = str_obj->GetUtf8(&utf8_data);
            if (DAS::IsFailed(result))
            {
                return result;
            }
            size_t utf8_len =
                (utf8_data != nullptr) ? std::strlen(utf8_data) : 0;
            // STRING wire format: [str_len:8B][utf8_bytes:str_len]
            writer.WriteUInt64(static_cast<uint64_t>(utf8_len));
            if (utf8_len > 0 && utf8_data != nullptr)
            {
                writer.Write(utf8_data, utf8_len);
            }
            return DAS_S_OK;
        }
        case DasVariantType::DAS_VARIANT_TYPE_BOOL:
        {
            bool      value = false;
            DasResult result = impl->GetBool(index, &value);
            if (DAS::IsFailed(result))
            {
                return result;
            }
            // BOOL wire format: 1 byte
            writer.WriteUInt8(value ? 1 : 0);
            return DAS_S_OK;
        }
        case DasVariantType::DAS_VARIANT_TYPE_BASE:
        {
            DAS::DasPtr<IDasBase> base_obj;
            DasResult             result = impl->GetBase(index, base_obj.Put());
            if (DAS::IsFailed(result))
            {
                return result;
            }
            // BASE wire format (12B):
            // [session_id:2B][generation:2B][local_id:4B][interface_id:4B]
            if (base_obj.Get() != nullptr)
            {
                ObjectId  oid;
                DasResult register_result =
                    ctx.object_manager.RegisterLocalObject(base_obj.Get(), oid);
                if (DAS::IsFailed(register_result))
                {
                    return register_result;
                }
                writer.WriteUInt16(oid.session_id);
                writer.WriteUInt16(oid.generation);
                writer.WriteUInt32(oid.local_id);
                writer.WriteUInt32(ComputeInterfaceId(DasIidOf<IDasBase>()));
            }
            else
            {
                writer.WriteUInt16(0); // session_id
                writer.WriteUInt16(0); // generation
                writer.WriteUInt32(0); // local_id
                writer.WriteUInt32(0); // interface_id
            }
            return DAS_S_OK;
        }
        case DasVariantType::DAS_VARIANT_TYPE_COMPONENT:
        {
            DAS::DasPtr<Das::PluginInterface::IDasComponent> comp_obj;
            DasResult result = impl->GetComponent(index, comp_obj.Put());
            if (DAS::IsFailed(result))
            {
                return result;
            }
            // COMPONENT wire format (12B): same as BASE
            if (comp_obj.Get() != nullptr)
            {
                ObjectId  oid;
                DasResult register_result =
                    ctx.object_manager.RegisterLocalObject(comp_obj.Get(), oid);
                if (DAS::IsFailed(register_result))
                {
                    return register_result;
                }
                writer.WriteUInt16(oid.session_id);
                writer.WriteUInt16(oid.generation);
                writer.WriteUInt32(oid.local_id);
                writer.WriteUInt32(ComputeInterfaceId(
                    DasIidOf<Das::PluginInterface::IDasComponent>()));
            }
            else
            {
                writer.WriteUInt16(0); // session_id
                writer.WriteUInt16(0); // generation
                writer.WriteUInt32(0); // local_id
                writer.WriteUInt32(0); // interface_id
            }
            return DAS_S_OK;
        }
        default:
            DAS_CORE_LOG_ERROR(
                "Unknown variant type = {}",
                static_cast<int>(type));
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
    }

} // anonymous namespace

DasResult DasVariantVectorByValueStub::DispatchMethod(
    uint16_t              method_id,
    void*                 impl,
    const uint8_t*        params,
    size_t                params_size,
    StubContext&          ctx,
    std::vector<uint8_t>& out_response)
{
    if (method_id <= 5 || (method_id >= 18 && method_id <= 20))
    {
        // Get methods (0-5, 18-20): return full VariantVector snapshot
        return HandleGetSnapshot(impl, ctx, out_response);
    }

    if (method_id >= 6 && method_id <= 11)
    {
        // Set methods (6-11): read index + value, call impl's Set method
        return HandleSet(method_id, impl, params, params_size, ctx);
    }

    if (method_id >= 12 && method_id <= 17)
    {
        // PushBack methods (12-17): read value, call impl's PushBack method
        return HandlePushBack(method_id, impl, params, params_size, ctx);
    }

    DAS_CORE_LOG_ERROR("Unknown method_id = {}", method_id);
    return DAS_E_IPC_UNKNOWN_METHOD;
}

DasResult DasVariantVectorByValueStub::HandleGetSnapshot(
    void*                 impl,
    StubContext&          ctx,
    std::vector<uint8_t>& out_response)
{
    auto* variant_vector =
        static_cast<Das::ExportInterface::IDasVariantVector*>(impl);

    // GetSize returns the size as DasResult directly.
    // Per IDL: "此函数一定成功，因此返回值即为大小"
    // DasResult is int32_t, so we cast it to uint64_t for count.
    DasResult size_result = variant_vector->GetSize();
    uint64_t  count =
        static_cast<uint64_t>(static_cast<uint64_t>(size_result) & 0xFFFFFFFF);

    // Serialize the full snapshot
    // Wire format: [count:8B][type1:4B][value1:varies]...
    MemorySerializerWriter writer;
    writer.WriteUInt64(count);

    for (uint64_t i = 0; i < count; ++i)
    {
        using Das::ExportInterface::DasVariantType;
        DasVariantType type = DasVariantType::DAS_VARIANT_TYPE_INT;
        DasResult      type_result = variant_vector->GetType(i, &type);
        if (DAS::IsFailed(type_result))
        {
            DAS_CORE_LOG_ERROR(
                "GetType failed at index = {}, result = {}",
                i,
                type_result);
            return type_result;
        }

        DasResult write_result =
            WriteVariantValue(writer, type, variant_vector, i, ctx);
        if (DAS::IsFailed(write_result))
        {
            return write_result;
        }
    }

    out_response = std::move(writer.GetBuffer());
    return DAS_S_OK;
}

DasResult DasVariantVectorByValueStub::HandleSet(
    uint16_t       method_id,
    void*          impl,
    const uint8_t* params,
    size_t         params_size,
    StubContext&   ctx)
{
    auto* variant_vector =
        static_cast<Das::ExportInterface::IDasVariantVector*>(impl);

    size_t offset = 0;

    // Set methods wire format: [index:8B][value:varies]
    uint64_t index = 0;
    if (!ReadUInt64(params, params_size, offset, index))
    {
        DAS_CORE_LOG_ERROR(
            "HandleSet: failed to read index, method_id = {}",
            method_id);
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    switch (method_id)
    {
    case 6: // SetInt
    {
        int64_t value = 0;
        if (!ReadInt64(params, params_size, offset, value))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        return variant_vector->SetInt(index, value);
    }
    case 7: // SetFloat
    {
        float value = 0.0f;
        if (!ReadFloat(params, params_size, offset, value))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        return variant_vector->SetFloat(index, value);
    }
    case 8: // SetString
    {
        std::string utf8_str;
        if (!ReadString(params, params_size, offset, utf8_str))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        // Create an IDasReadOnlyString from the UTF-8 data
        DAS::DasPtr<IDasReadOnlyString> str_obj;
        DasResult result = CreateIDasReadOnlyStringFromUtf8WithLength(
            utf8_str.c_str(),
            utf8_str.size(),
            str_obj.Put());
        if (DAS::IsFailed(result))
        {
            return result;
        }
        return variant_vector->SetString(index, str_obj.Get());
    }
    case 9: // SetBool
    {
        bool value = false;
        if (!ReadBool(params, params_size, offset, value))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        return variant_vector->SetBool(index, value);
    }
    case 10: // SetComponent — COMPONENT wire format (12B)
    {
        uint16_t session_id = 0;
        uint16_t generation = 0;
        uint32_t local_id = 0;
        uint32_t iface_id = 0;
        if (!ReadUInt16(params, params_size, offset, session_id)
            || !ReadUInt16(params, params_size, offset, generation)
            || !ReadUInt32(params, params_size, offset, local_id)
            || !ReadUInt32(params, params_size, offset, iface_id))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        ObjectId oid{session_id, generation, local_id};
        if (IsNullObjectId(oid))
        {
            return variant_vector->SetComponent(index, nullptr);
        }
        IDasBase* base_ptr = nullptr;
        DasResult result = DeserializeInInterfaceParam(
            EncodeObjectId(oid),
            iface_id,
            ctx.object_manager,
            ctx.run_loop,
            ctx.business_thread,
            &base_ptr);
        if (DAS::IsFailed(result))
        {
            return result;
        }
        auto* component =
            static_cast<Das::PluginInterface::IDasComponent*>(base_ptr);
        return variant_vector->SetComponent(index, component);
    }
    case 11: // SetBase — BASE wire format (12B)
    {
        uint16_t session_id = 0;
        uint16_t generation = 0;
        uint32_t local_id = 0;
        uint32_t iface_id = 0;
        if (!ReadUInt16(params, params_size, offset, session_id)
            || !ReadUInt16(params, params_size, offset, generation)
            || !ReadUInt32(params, params_size, offset, local_id)
            || !ReadUInt32(params, params_size, offset, iface_id))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        ObjectId oid{session_id, generation, local_id};
        if (IsNullObjectId(oid))
        {
            return variant_vector->SetBase(index, nullptr);
        }
        IDasBase* base_ptr = nullptr;
        DasResult result = DeserializeInInterfaceParam(
            EncodeObjectId(oid),
            iface_id,
            ctx.object_manager,
            ctx.run_loop,
            ctx.business_thread,
            &base_ptr);
        if (DAS::IsFailed(result))
        {
            return result;
        }
        return variant_vector->SetBase(index, base_ptr);
    }
    default:
        DAS_CORE_LOG_ERROR("HandleSet: unexpected method_id = {}", method_id);
        return DAS_E_IPC_INVALID_ARGUMENT;
    }
}

DasResult DasVariantVectorByValueStub::HandlePushBack(
    uint16_t       method_id,
    void*          impl,
    const uint8_t* params,
    size_t         params_size,
    StubContext&   ctx)
{
    auto* variant_vector =
        static_cast<Das::ExportInterface::IDasVariantVector*>(impl);

    size_t offset = 0;

    // PushBack methods wire format: [value:varies] (no index)
    switch (method_id)
    {
    case 12: // PushBackInt
    {
        int64_t value = 0;
        if (!ReadInt64(params, params_size, offset, value))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        return variant_vector->PushBackInt(value);
    }
    case 13: // PushBackFloat
    {
        float value = 0.0f;
        if (!ReadFloat(params, params_size, offset, value))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        return variant_vector->PushBackFloat(value);
    }
    case 14: // PushBackString
    {
        std::string utf8_str;
        if (!ReadString(params, params_size, offset, utf8_str))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        DAS::DasPtr<IDasReadOnlyString> str_obj;
        DasResult result = CreateIDasReadOnlyStringFromUtf8WithLength(
            utf8_str.c_str(),
            utf8_str.size(),
            str_obj.Put());
        if (DAS::IsFailed(result))
        {
            return result;
        }
        return variant_vector->PushBackString(str_obj.Get());
    }
    case 15: // PushBackBool
    {
        bool value = false;
        if (!ReadBool(params, params_size, offset, value))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        return variant_vector->PushBackBool(value);
    }
    case 16: // PushBackComponent
    {
        uint16_t session_id = 0;
        uint16_t generation = 0;
        uint32_t local_id = 0;
        uint32_t iface_id = 0;
        if (!ReadUInt16(params, params_size, offset, session_id)
            || !ReadUInt16(params, params_size, offset, generation)
            || !ReadUInt32(params, params_size, offset, local_id)
            || !ReadUInt32(params, params_size, offset, iface_id))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        ObjectId oid{session_id, generation, local_id};
        if (IsNullObjectId(oid))
        {
            return variant_vector->PushBackComponent(nullptr);
        }
        IDasBase* base_ptr = nullptr;
        DasResult result = DeserializeInInterfaceParam(
            EncodeObjectId(oid),
            iface_id,
            ctx.object_manager,
            ctx.run_loop,
            ctx.business_thread,
            &base_ptr);
        if (DAS::IsFailed(result))
        {
            return result;
        }
        auto* component =
            static_cast<Das::PluginInterface::IDasComponent*>(base_ptr);
        return variant_vector->PushBackComponent(component);
    }
    case 17: // PushBackBase
    {
        uint16_t session_id = 0;
        uint16_t generation = 0;
        uint32_t local_id = 0;
        uint32_t iface_id = 0;
        if (!ReadUInt16(params, params_size, offset, session_id)
            || !ReadUInt16(params, params_size, offset, generation)
            || !ReadUInt32(params, params_size, offset, local_id)
            || !ReadUInt32(params, params_size, offset, iface_id))
        {
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }
        ObjectId oid{session_id, generation, local_id};
        if (IsNullObjectId(oid))
        {
            return variant_vector->PushBackBase(nullptr);
        }
        IDasBase* base_ptr = nullptr;
        DasResult result = DeserializeInInterfaceParam(
            EncodeObjectId(oid),
            iface_id,
            ctx.object_manager,
            ctx.run_loop,
            ctx.business_thread,
            &base_ptr);
        if (DAS::IsFailed(result))
        {
            return result;
        }
        return variant_vector->PushBackBase(base_ptr);
    }
    default:
        DAS_CORE_LOG_ERROR(
            "HandlePushBack: unexpected method_id = {}",
            method_id);
        return DAS_E_IPC_INVALID_ARGUMENT;
    }
}

DAS_CORE_IPC_NS_END
