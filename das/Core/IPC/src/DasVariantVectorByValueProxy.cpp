#include <das/Core/IPC/DasVariantVectorByValueProxy.h>

#include <cstring>
#include <das/Core/IPC/InterfaceParamSerialization.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/ipc/IpcProxyFactory.h>
#include <stdexec/execution.hpp>
#include <tuple>

DAS_CORE_IPC_NS_BEGIN

// ============================================================
// Method ID mapping (0-25):
//   0: GetInt,        1: GetFloat,       2: GetString
//   3: GetBool,       4: GetComponent,   5: GetBase
//   6: SetInt,        7: SetFloat,       8: SetString
//   9: SetBool,      10: SetComponent,  11: SetBase
//  12: PushBackInt,  13: PushBackFloat, 14: PushBackString
//  15: PushBackBool, 16: PushBackComponent, 17: PushBackBase
//  18: GetType,      19: RemoveAt,      20: GetSize
//  21: GetImage,     22: IsNull
//  23: SetImage
//  24: PushBackImage, 25: PushBackNull
// ============================================================

DasVariantVectorByValueProxy::DasVariantVectorByValueProxy(
    uint32_t                      interface_id,
    const ObjectId&               object_id,
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    ProxyFactory&                 proxy_factory)
    : DasProxyBase<IDasVariantVector>(
          interface_id,
          object_id,
          run_loop,
          std::move(business_thread),
          proxy_factory)
{
}

// ── QueryInterface ────────────────────────────────────────────────────────

DasResult DasVariantVectorByValueProxy::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    if (pp_object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    if (iid == DAS_IID_VARIANT_VECTOR)
    {
        *pp_object = static_cast<IDasVariantVector*>(this);
        AddRef();
        return DAS_S_OK;
    }

    // Try remote QI for other interfaces
    return QueryInterfaceRemote(iid, pp_object);
}

// ── EnsureDataLoaded ──────────────────────────────────────────────────────

DasResult DasVariantVectorByValueProxy::EnsureDataLoaded(uint16_t method_id)
{
    if (data_loaded_)
    {
        return DAS_S_OK;
    }

    DasResult runtime_result =
        CheckRuntimeAvailable("DasVariantVectorByValueProxy::EnsureDataLoaded");
    if (DAS::IsFailed(runtime_result))
    {
        return runtime_result;
    }

    // Build business body with the method_id (stub ignores it for Get, but
    // includes it for logging/routing)
    auto body = BuildBusinessBody(method_id);

    // We need to manually build a REQUEST header with BUSINESS_EVENT flag.
    // IPCProxyBase::SendRequest() uses BuildRequestHeader() which does NOT
    // set header_flags, so we replicate SendRequest logic here with the
    // correct flag.
    auto bt = GetBusinessThread().lock();
    if (!bt)
    {
        DAS_CORE_LOG_ERROR("BusinessThread not available");
        return DAS_E_IPC_DISCONNECTED;
    }

    uint16_t call_id = NextCallId();

    ValidatedIPCMessageHeader header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetHeaderFlags(HeaderFlags::BUSINESS_EVENT)
            .SetBodySize(static_cast<uint32_t>(body.size()))
            .SetCallId(call_id)
            .SetInterfaceId(GetInterfaceId())
            .SetSourceSessionId(GetSourceSessionId())
            .SetTargetSessionId(GetObjectId().session_id)
            .Build();

    CallKey call_key{GetObjectId().session_id, call_id};

    std::vector<uint8_t> response;

    if (bt->IsCurrentThread())
    {
        // Nested pump: called from within BusinessThread
        DasResult send_result = GetRunLoop().PostSend(header, std::move(body));
        if (send_result != DAS_S_OK)
        {
            DAS_CORE_LOG_ERROR("PostSend failed, result = {}", send_result);
            return send_result;
        }

        DasResult pump_result = bt->PumpUntilResponse(call_key, response);
        if (DAS::IsFailed(pump_result))
        {
            DAS_CORE_LOG_ERROR(
                "PumpUntilResponse failed, result = {}",
                pump_result);
            return pump_result;
        }
    }
    else
    {
        // External thread path: AwaitResponseSender 的 start() 会调用
        // PostSend(带回调)，在 IO 线程注册 pending call 后再发送
        constexpr auto kTimeout = std::chrono::milliseconds{30000};

        AwaitResponseSender
             sender{&GetRunLoop(), header, std::move(body), call_key, kTimeout};
        auto result = stdexec::sync_wait(std::move(sender));
        if (!result)
        {
            DAS_CORE_LOG_ERROR("sync_wait failed for call_id = {}", call_id);
            return DAS_E_IPC_REMOTE_ERROR;
        }

        auto&     inner = std::get<0>(*result);
        DasResult ipc_result = std::get<0>(inner);
        response = std::move(std::get<1>(inner));

        if (DAS::IsFailed(ipc_result))
        {
            DAS_CORE_LOG_ERROR("IPC request failed, result = {}", ipc_result);
            return ipc_result;
        }
    }

    // Deserialize snapshot response:
    // Wire format: [count:8B][type1:4B][value1:varies][type2:4B]...
    MemorySerializerReader reader(response);
    uint64_t               count = 0;
    DasResult              result = reader.ReadUInt64(&count);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("Failed to read count from response");
        return result;
    }

    cache_.clear();
    cache_.reserve(static_cast<size_t>(count));

    using Das::ExportInterface::DasVariantType;

    for (uint64_t i = 0; i < count; ++i)
    {
        CachedVariant entry;

        uint32_t type_raw = 0;
        result = reader.ReadUInt32(&type_raw);
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_ERROR("Failed to read type at index = {}", i);
            cache_.clear();
            return result;
        }
        entry.type = static_cast<DasVariantType>(type_raw);

        switch (entry.type)
        {
        case DasVariantType::DAS_VARIANT_TYPE_INT:
        {
            result = reader.ReadInt64(&entry.int_value);
            if (DAS::IsFailed(result))
            {
                DAS_CORE_LOG_ERROR("Failed to read INT value at index = {}", i);
                cache_.clear();
                return result;
            }
            break;
        }
        case DasVariantType::DAS_VARIANT_TYPE_FLOAT:
        {
            result = reader.ReadFloat(&entry.float_value);
            if (DAS::IsFailed(result))
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to read FLOAT value at index = {}",
                    i);
                cache_.clear();
                return result;
            }
            break;
        }
        case DasVariantType::DAS_VARIANT_TYPE_STRING:
        {
            // STRING wire format: [str_len:8B][utf8_bytes:str_len]
            result = reader.ReadString(entry.string_value);
            if (DAS::IsFailed(result))
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to read STRING value at index = {}",
                    i);
                cache_.clear();
                return result;
            }
            break;
        }
        case DasVariantType::DAS_VARIANT_TYPE_BOOL:
        {
            uint8_t raw = 0;
            result = reader.ReadUInt8(&raw);
            if (DAS::IsFailed(result))
            {
                DAS_CORE_LOG_ERROR(
                    "Failed to read BOOL value at index = {}",
                    i);
                cache_.clear();
                return result;
            }
            entry.bool_value = (raw != 0);
            break;
        }
        case DasVariantType::DAS_VARIANT_TYPE_BASE:
        case DasVariantType::DAS_VARIANT_TYPE_COMPONENT:
        case DasVariantType::DAS_VARIANT_TYPE_IMAGE:
        {
            // BASE/COMPONENT wire format (12B):
            // [session_id:2B][generation:2B][local_id:4B][interface_id:4B]
            uint16_t session_id = 0;
            uint16_t generation = 0;
            uint32_t local_id = 0;
            uint32_t iface_id = 0;
            result = reader.ReadUInt16(&session_id);
            if (DAS::IsFailed(result))
            {
                cache_.clear();
                return result;
            }
            result = reader.ReadUInt16(&generation);
            if (DAS::IsFailed(result))
            {
                cache_.clear();
                return result;
            }
            result = reader.ReadUInt32(&local_id);
            if (DAS::IsFailed(result))
            {
                cache_.clear();
                return result;
            }
            result = reader.ReadUInt32(&iface_id);
            if (DAS::IsFailed(result))
            {
                cache_.clear();
                return result;
            }
            entry.object_id = ObjectId{session_id, generation, local_id};
            entry.interface_id = iface_id;
            break;
        }
        case DasVariantType::DAS_VARIANT_TYPE_NULL:
        {
            // NULL has no value payload
            break;
        }
        default:
            DAS_CORE_LOG_ERROR(
                "Unknown variant type = {} at index = {}",
                type_raw,
                i);
            cache_.clear();
            return DAS_E_IPC_DESERIALIZATION_FAILED;
        }

        cache_.push_back(std::move(entry));
    }

    // Second pass: create proxies for BASE/COMPONENT entries
    for (size_t idx = 0; idx < cache_.size(); ++idx)
    {
        auto& entry = cache_[idx];
        if ((entry.type == DasVariantType::DAS_VARIANT_TYPE_BASE
             || entry.type == DasVariantType::DAS_VARIANT_TYPE_COMPONENT
             || entry.type == DasVariantType::DAS_VARIANT_TYPE_IMAGE)
            && (entry.object_id.session_id != 0
                || entry.object_id.local_id != 0))
        {
            runtime_result = CheckRuntimeAvailable(
                "DasVariantVectorByValueProxy::EnsureDataLoaded");
            if (DAS::IsFailed(runtime_result))
            {
                cache_.clear();
                return runtime_result;
            }

            auto [create_result, proxy] = GetProxyFactory().GetOrCreateProxy(
                GetRunLoop(),
                GetBusinessThread(),
                entry.object_id,
                entry.interface_id);
            if (proxy)
            {
                entry.base_ptr = std::move(proxy);
            }
            else
            {
                DAS_CORE_LOG_ERROR(
                    "EnsureDataLoaded: GetOrCreateProxy failed, result = {}",
                    create_result);
                cache_.clear();
                return create_result;
            }
        }
    }

    data_loaded_ = true;
    return DAS_S_OK;
}

DasResult DasVariantVectorByValueProxy::SendWriteBack(
    uint16_t       method_id,
    const uint8_t* params,
    size_t         params_size)
{
    DasResult runtime_result =
        CheckRuntimeAvailable("DasVariantVectorByValueProxy::SendWriteBack");
    if (DAS::IsFailed(runtime_result))
    {
        return runtime_result;
    }

    // Build body: standard V3 body header (interface_id + method_id +
    // reserved + ObjectId) + write params
    auto body = BuildBusinessBody(method_id, params, params_size);

    // Fire-and-forget EVENT + BUSINESS_EVENT
    ValidatedIPCMessageHeader header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::EVENT)
            .SetHeaderFlags(HeaderFlags::BUSINESS_EVENT)
            .SetBodySize(static_cast<uint32_t>(body.size()))
            .SetInterfaceId(GetInterfaceId())
            .SetSourceSessionId(GetSourceSessionId())
            .SetTargetSessionId(GetObjectId().session_id)
            .Build();

    return GetRunLoop().PostSend(header, std::move(body));
}

// ── Read methods ──────────────────────────────────────────────────────────

DasResult DasVariantVectorByValueProxy::GetInt(
    uint64_t index,
    int64_t* p_out_int)
{
    if (p_out_int == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    DasResult result = EnsureDataLoaded(0);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    if (cache_[index].type != ::Das::ExportInterface::DAS_VARIANT_TYPE_INT)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    *p_out_int = cache_[index].int_value;
    return DAS_S_OK;
}

DasResult DasVariantVectorByValueProxy::GetFloat(
    uint64_t index,
    float*   p_out_float)
{
    if (p_out_float == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    DasResult result = EnsureDataLoaded(1);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    if (cache_[index].type != ::Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    *p_out_float = cache_[index].float_value;
    return DAS_S_OK;
}

DasResult DasVariantVectorByValueProxy::GetString(
    uint64_t             index,
    IDasReadOnlyString** pp_out_string)
{
    if (pp_out_string == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_string = nullptr;

    DasResult result = EnsureDataLoaded(2);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    if (cache_[index].type != ::Das::ExportInterface::DAS_VARIANT_TYPE_STRING)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // Create a local IDasReadOnlyString from the cached string data
    result = CreateIDasReadOnlyStringFromUtf8WithLength(
        cache_[index].string_value.c_str(),
        cache_[index].string_value.size(),
        pp_out_string);
    return result;
}

DasResult DasVariantVectorByValueProxy::GetBool(
    uint64_t index,
    bool*    p_out_bool)
{
    if (p_out_bool == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    DasResult result = EnsureDataLoaded(3);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    if (cache_[index].type != ::Das::ExportInterface::DAS_VARIANT_TYPE_BOOL)
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    *p_out_bool = cache_[index].bool_value;
    return DAS_S_OK;
}

DasResult DasVariantVectorByValueProxy::GetComponent(
    uint64_t                                index,
    ::Das::PluginInterface::IDasComponent** pp_out_component)
{
    if (pp_out_component == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_component = nullptr;

    DasResult result = EnsureDataLoaded(4);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    if (cache_[index].type
        != ::Das::ExportInterface::DAS_VARIANT_TYPE_COMPONENT)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    if (cache_[index].base_ptr.Get() != nullptr)
    {
        cache_[index].base_ptr.Get()->AddRef();
        *pp_out_component = static_cast<Das::PluginInterface::IDasComponent*>(
            cache_[index].base_ptr.Get());
    }
    return DAS_S_OK;
}

DasResult DasVariantVectorByValueProxy::GetBase(
    uint64_t     index,
    ::IDasBase** pp_out_base)
{
    if (pp_out_base == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_base = nullptr;

    DasResult result = EnsureDataLoaded(5);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    if (cache_[index].type != ::Das::ExportInterface::DAS_VARIANT_TYPE_BASE)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    if (cache_[index].base_ptr.Get() != nullptr)
    {
        cache_[index].base_ptr.Get()->AddRef();
        *pp_out_base = cache_[index].base_ptr.Get();
    }
    return DAS_S_OK;
}

DasResult DasVariantVectorByValueProxy::GetImage(
    uint64_t                          index,
    Das::ExportInterface::IDasImage** pp_out_image)
{
    if (pp_out_image == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_image = nullptr;
    (void)index;
    // IMAGE IPC proxy not yet implemented — requires wire format extension
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult DasVariantVectorByValueProxy::IsNull(
    uint64_t index,
    bool*    out_is_null)
{
    if (out_is_null == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    DasResult result = EnsureDataLoaded(22);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    *out_is_null =
        (cache_[index].type == ::Das::ExportInterface::DAS_VARIANT_TYPE_NULL);
    return DAS_S_OK;
}

DasResult DasVariantVectorByValueProxy::GetType(
    uint64_t                                index,
    ::Das::ExportInterface::DasVariantType* p_out_type)
{
    if (p_out_type == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    DasResult result = EnsureDataLoaded(18);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }
    *p_out_type = cache_[index].type;
    return DAS_S_OK;
}

DasResult DasVariantVectorByValueProxy::GetSize()
{
    DasResult result = EnsureDataLoaded(20);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    // GetSize returns size as DasResult per IDL convention
    return static_cast<DasResult>(cache_.size());
}

// ── Write methods: update local cache + fire-and-forget write-back ────────

DasResult DasVariantVectorByValueProxy::SetInt(uint64_t index, int64_t in_int)
{
    DasResult result = EnsureDataLoaded(6);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // Update local cache
    cache_[index].type = ::Das::ExportInterface::DAS_VARIANT_TYPE_INT;
    cache_[index].int_value = in_int;

    // Fire-and-forget: [index:8B][value:8B]
    MemorySerializerWriter writer;
    writer.WriteUInt64(index);
    writer.WriteInt64(in_int);
    return SendWriteBack(
        6,
        writer.GetBuffer().data(),
        writer.GetBuffer().size());
}

DasResult DasVariantVectorByValueProxy::SetFloat(uint64_t index, float in_float)
{
    DasResult result = EnsureDataLoaded(7);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    cache_[index].type = ::Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT;
    cache_[index].float_value = in_float;

    // [index:8B][value:4B]
    MemorySerializerWriter writer;
    writer.WriteUInt64(index);
    writer.WriteFloat(in_float);
    return SendWriteBack(
        7,
        writer.GetBuffer().data(),
        writer.GetBuffer().size());
}

DasResult DasVariantVectorByValueProxy::SetString(
    uint64_t            index,
    IDasReadOnlyString* in_string)
{
    if (in_string == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    DasResult result = EnsureDataLoaded(8);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // Read string value from the input interface
    const char* utf8_data = nullptr;
    result = in_string->GetUtf8(&utf8_data);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    size_t utf8_len = (utf8_data != nullptr) ? std::strlen(utf8_data) : 0;

    // Update local cache
    cache_[index].type = ::Das::ExportInterface::DAS_VARIANT_TYPE_STRING;
    cache_[index].string_value.assign(utf8_data, utf8_len);

    // Fire-and-forget: [index:8B][str_len:8B][utf8_bytes:str_len]
    MemorySerializerWriter writer;
    writer.WriteUInt64(index);
    writer.WriteUInt64(static_cast<uint64_t>(utf8_len));
    if (utf8_len > 0 && utf8_data != nullptr)
    {
        writer.Write(utf8_data, utf8_len);
    }
    return SendWriteBack(
        8,
        writer.GetBuffer().data(),
        writer.GetBuffer().size());
}

DasResult DasVariantVectorByValueProxy::SetBool(uint64_t index, bool in_bool)
{
    DasResult result = EnsureDataLoaded(9);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    cache_[index].type = ::Das::ExportInterface::DAS_VARIANT_TYPE_BOOL;
    cache_[index].bool_value = in_bool;

    // [index:8B][value:1B]
    MemorySerializerWriter writer;
    writer.WriteUInt64(index);
    writer.WriteUInt8(in_bool ? 1 : 0);
    return SendWriteBack(
        9,
        writer.GetBuffer().data(),
        writer.GetBuffer().size());
}

DasResult DasVariantVectorByValueProxy::SetComponent(
    uint64_t                               index,
    ::Das::PluginInterface::IDasComponent* in_component)
{
    DasResult result = EnsureDataLoaded(10);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    PendingInParamExportGuard guard{GetObjectManager()};

    // Serialize interface pointer to ObjectId
    ObjectId oid{};
    bool     newly_registered = false;
    result = SerializeInInterfaceParam(
        in_component,
        GetObjectManager(),
        oid,
        &newly_registered);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    guard.Track(oid, newly_registered);

    // Update local cache
    cache_[index].type = ::Das::ExportInterface::DAS_VARIANT_TYPE_COMPONENT;
    cache_[index].object_id = oid;
    cache_[index].interface_id =
        ComputeInterfaceId(DasIidOf<Das::PluginInterface::IDasComponent>());
    cache_[index].base_ptr = DasPtr<IDasBase>::Attach(in_component);
    if (in_component != nullptr)
    {
        in_component->AddRef();
    }

    // Fire-and-forget:
    // [index:8B][session_id:2B][generation:2B][local_id:4B][interface_id:4B]
    MemorySerializerWriter writer;
    writer.WriteUInt64(index);
    writer.WriteUInt16(oid.session_id);
    writer.WriteUInt16(oid.generation);
    writer.WriteUInt32(oid.local_id);
    writer.WriteUInt32(cache_[index].interface_id);
    auto send_result =
        SendWriteBack(10, writer.GetBuffer().data(), writer.GetBuffer().size());
    if (!IsTransportLevelError(send_result))
    {
        guard.Commit();
    }
    return send_result;
}

DasResult DasVariantVectorByValueProxy::SetBase(
    uint64_t    index,
    ::IDasBase* in_base)
{
    DasResult result = EnsureDataLoaded(11);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    PendingInParamExportGuard guard{GetObjectManager()};

    ObjectId oid{};
    bool     newly_registered = false;
    result = SerializeInInterfaceParam(
        in_base,
        GetObjectManager(),
        oid,
        &newly_registered);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    guard.Track(oid, newly_registered);

    cache_[index].type = ::Das::ExportInterface::DAS_VARIANT_TYPE_BASE;
    cache_[index].object_id = oid;
    cache_[index].interface_id = ComputeInterfaceId(DasIidOf<IDasBase>());
    cache_[index].base_ptr = DasPtr<IDasBase>::Attach(in_base);
    if (in_base != nullptr)
    {
        in_base->AddRef();
    }

    // Fire-and-forget:
    // [index:8B][session_id:2B][generation:2B][local_id:4B][interface_id:4B]
    MemorySerializerWriter writer;
    writer.WriteUInt64(index);
    writer.WriteUInt16(oid.session_id);
    writer.WriteUInt16(oid.generation);
    writer.WriteUInt32(oid.local_id);
    writer.WriteUInt32(cache_[index].interface_id);
    auto send_result =
        SendWriteBack(11, writer.GetBuffer().data(), writer.GetBuffer().size());
    if (!IsTransportLevelError(send_result))
    {
        guard.Commit();
    }
    return send_result;
}

DasResult DasVariantVectorByValueProxy::SetImage(
    uint64_t                         index,
    Das::ExportInterface::IDasImage* p_image)
{
    (void)index;
    (void)p_image;
    // IMAGE IPC proxy not yet implemented
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult DasVariantVectorByValueProxy::PushBackInt(int64_t in_int)
{
    DasResult result = EnsureDataLoaded(12);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    // Update local cache
    CachedVariant entry;
    entry.type = ::Das::ExportInterface::DAS_VARIANT_TYPE_INT;
    entry.int_value = in_int;
    cache_.push_back(std::move(entry));

    // Fire-and-forget: [value:8B]
    MemorySerializerWriter writer;
    writer.WriteInt64(in_int);
    return SendWriteBack(
        12,
        writer.GetBuffer().data(),
        writer.GetBuffer().size());
}

DasResult DasVariantVectorByValueProxy::PushBackFloat(float in_float)
{
    DasResult result = EnsureDataLoaded(13);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    CachedVariant entry;
    entry.type = ::Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT;
    entry.float_value = in_float;
    cache_.push_back(std::move(entry));

    // [value:4B]
    MemorySerializerWriter writer;
    writer.WriteFloat(in_float);
    return SendWriteBack(
        13,
        writer.GetBuffer().data(),
        writer.GetBuffer().size());
}

DasResult DasVariantVectorByValueProxy::PushBackString(
    IDasReadOnlyString* in_string)
{
    if (in_string == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    DasResult result = EnsureDataLoaded(14);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    const char* utf8_data = nullptr;
    result = in_string->GetUtf8(&utf8_data);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    size_t utf8_len = (utf8_data != nullptr) ? std::strlen(utf8_data) : 0;

    CachedVariant entry;
    entry.type = ::Das::ExportInterface::DAS_VARIANT_TYPE_STRING;
    entry.string_value.assign(utf8_data, utf8_len);
    cache_.push_back(std::move(entry));

    // [str_len:8B][utf8_bytes:str_len]
    MemorySerializerWriter writer;
    writer.WriteUInt64(static_cast<uint64_t>(utf8_len));
    if (utf8_len > 0 && utf8_data != nullptr)
    {
        writer.Write(utf8_data, utf8_len);
    }
    return SendWriteBack(
        14,
        writer.GetBuffer().data(),
        writer.GetBuffer().size());
}

DasResult DasVariantVectorByValueProxy::PushBackBool(bool in_bool)
{
    DasResult result = EnsureDataLoaded(15);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    CachedVariant entry;
    entry.type = ::Das::ExportInterface::DAS_VARIANT_TYPE_BOOL;
    entry.bool_value = in_bool;
    cache_.push_back(std::move(entry));

    // [value:1B]
    MemorySerializerWriter writer;
    writer.WriteUInt8(in_bool ? 1 : 0);
    return SendWriteBack(
        15,
        writer.GetBuffer().data(),
        writer.GetBuffer().size());
}

DasResult DasVariantVectorByValueProxy::PushBackComponent(
    ::Das::PluginInterface::IDasComponent* in_component)
{
    DasResult result = EnsureDataLoaded(16);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    PendingInParamExportGuard guard{GetObjectManager()};

    ObjectId oid{};
    bool     newly_registered = false;
    result = SerializeInInterfaceParam(
        in_component,
        GetObjectManager(),
        oid,
        &newly_registered);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    guard.Track(oid, newly_registered);

    CachedVariant entry;
    entry.type = ::Das::ExportInterface::DAS_VARIANT_TYPE_COMPONENT;
    entry.object_id = oid;
    entry.interface_id =
        ComputeInterfaceId(DasIidOf<Das::PluginInterface::IDasComponent>());
    entry.base_ptr = DasPtr<IDasBase>::Attach(in_component);
    if (in_component != nullptr)
    {
        in_component->AddRef();
    }
    cache_.push_back(std::move(entry));

    // Fire-and-forget:
    // [session_id:2B][generation:2B][local_id:4B][interface_id:4B]
    MemorySerializerWriter writer;
    writer.WriteUInt16(oid.session_id);
    writer.WriteUInt16(oid.generation);
    writer.WriteUInt32(oid.local_id);
    writer.WriteUInt32(
        ComputeInterfaceId(DasIidOf<Das::PluginInterface::IDasComponent>()));
    auto send_result =
        SendWriteBack(16, writer.GetBuffer().data(), writer.GetBuffer().size());
    if (!IsTransportLevelError(send_result))
    {
        guard.Commit();
    }
    return send_result;
}

DasResult DasVariantVectorByValueProxy::PushBackBase(::IDasBase* in_base)
{
    DasResult result = EnsureDataLoaded(17);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    PendingInParamExportGuard guard{GetObjectManager()};

    ObjectId oid{};
    bool     newly_registered = false;
    result = SerializeInInterfaceParam(
        in_base,
        GetObjectManager(),
        oid,
        &newly_registered);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    guard.Track(oid, newly_registered);

    CachedVariant entry;
    entry.type = ::Das::ExportInterface::DAS_VARIANT_TYPE_BASE;
    entry.object_id = oid;
    entry.interface_id = ComputeInterfaceId(DasIidOf<IDasBase>());
    entry.base_ptr = DasPtr<IDasBase>::Attach(in_base);
    if (in_base != nullptr)
    {
        in_base->AddRef();
    }
    cache_.push_back(std::move(entry));

    // Fire-and-forget:
    // [session_id:2B][generation:2B][local_id:4B][interface_id:4B]
    MemorySerializerWriter writer;
    writer.WriteUInt16(oid.session_id);
    writer.WriteUInt16(oid.generation);
    writer.WriteUInt32(oid.local_id);
    writer.WriteUInt32(ComputeInterfaceId(DasIidOf<IDasBase>()));
    auto send_result =
        SendWriteBack(17, writer.GetBuffer().data(), writer.GetBuffer().size());
    if (!IsTransportLevelError(send_result))
    {
        guard.Commit();
    }
    return send_result;
}

DasResult DasVariantVectorByValueProxy::PushBackImage(
    Das::ExportInterface::IDasImage* p_image)
{
    (void)p_image;
    // IMAGE IPC proxy not yet implemented
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult DasVariantVectorByValueProxy::PushBackNull()
{
    DasResult result = EnsureDataLoaded(25);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    CachedVariant entry;
    entry.type = ::Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
    cache_.push_back(std::move(entry));

    // Fire-and-forget: no payload for NULL
    return SendWriteBack(25, nullptr, 0);
}

DasResult DasVariantVectorByValueProxy::RemoveAt(uint64_t index)
{
    DasResult result = EnsureDataLoaded(19);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (index >= cache_.size())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // Update local cache
    cache_.erase(cache_.begin() + static_cast<ptrdiff_t>(index));

    // Fire-and-forget: [index:8B]
    MemorySerializerWriter writer;
    writer.WriteUInt64(index);
    return SendWriteBack(
        19,
        writer.GetBuffer().data(),
        writer.GetBuffer().size());
}

DAS_CORE_IPC_NS_END
