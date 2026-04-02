#include <das/Core/IPC/DasReadOnlyStringProxy.h>

#include <das/Core/Logger/Logger.h>
#include <unicode/unistr.h>
#include <unicode/ustring.h>
#include <unicode/utypes.h>

#include <cstring>

DAS_CORE_IPC_NS_BEGIN

DasReadOnlyStringProxy::DasReadOnlyStringProxy(
    uint32_t                      interface_id,
    const ObjectId&               object_id,
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    DistributedObjectManager&     object_manager)
    : DasProxyBase<IDasReadOnlyString>(
          interface_id,
          object_id,
          run_loop,
          std::move(business_thread),
          object_manager)
{
}

DasResult DasReadOnlyStringProxy::EnsureUtf16Loaded()
{
    if (utf16_ready_)
    {
        return DAS_S_OK;
    }

    // Build request body (V3: interface_id + method_id + reserved + ObjectId)
    // method_id = 0, no extra params
    auto body = BuildBusinessBody(0, nullptr, 0);

    std::vector<uint8_t> response;
    DasResult result = SendRequest(0, body.data(), body.size(), response);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("SendRequest failed with result = {}", result);
        return result;
    }

    // Deserialize response:
    // [result:4B][char_count:8B][utf16_bytes:char_count*2]
    MemorySerializerReader reader(response);
    int32_t                rpc_result = 0;
    uint64_t               char_count = 0;
    result = reader.ReadInt32(&rpc_result);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("Failed to read rpc_result from response");
        return result;
    }

    result = reader.ReadUInt64(&char_count);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("Failed to read char_count from response");
        return result;
    }

    DasResult final_result = static_cast<DasResult>(rpc_result);
    if (DAS::IsFailed(final_result))
    {
        DAS_CORE_LOG_ERROR("Remote returned error = {}", final_result);
        return final_result;
    }

    utf16_buffer_.resize(static_cast<size_t>(char_count));
    if (char_count > 0)
    {
        const uint8_t* raw_ptr = nullptr;
        result = reader.ReadRawPointer(
            &raw_ptr,
            static_cast<size_t>(char_count) * 2);
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_ERROR("Failed to read UTF-16 data from response");
            utf16_buffer_.clear();
            return result;
        }
        std::memcpy(
            utf16_buffer_.data(),
            raw_ptr,
            static_cast<size_t>(char_count) * 2);
    }

    utf16_ready_ = true;
    return DAS_S_OK;
}

DasResult DasReadOnlyStringProxy::EnsureUtf8Derived()
{
    if (utf8_ready_)
    {
        return DAS_S_OK;
    }

    // utf16_buffer_ must already be loaded
    icu::UnicodeString icu_str(
        reinterpret_cast<const UChar*>(utf16_buffer_.data()),
        static_cast<int32_t>(utf16_buffer_.size()));
    utf8_buffer_.clear();
    icu_str.toUTF8String(utf8_buffer_);

    utf8_ready_ = true;
    return DAS_S_OK;
}

DasResult DasReadOnlyStringProxy::EnsureWDerived()
{
    if (w_ready_)
    {
        return DAS_S_OK;
    }

#ifdef DAS_WINDOWS
    // Windows: wchar_t is 2 bytes, same as char16_t — direct copy
    w_buffer_.assign(utf16_buffer_.begin(), utf16_buffer_.end());
    w_ready_ = true;
    return DAS_S_OK;
#else
    // Linux: wchar_t is 4 bytes, convert UTF-16 → UTF-32 via ICU
    icu::UnicodeString icu_str(
        reinterpret_cast<const UChar*>(utf16_buffer_.data()),
        static_cast<int32_t>(utf16_buffer_.size()));

    UErrorCode error_code = U_ZERO_ERROR;
    int32_t    dest_capacity = 0;

    // Pre-flight to get required capacity
    u_strToWCS(
        nullptr,
        0,
        &dest_capacity,
        icu_str.getBuffer(),
        icu_str.length(),
        &error_code);
    error_code = U_ZERO_ERROR;

    w_buffer_.resize(static_cast<size_t>(dest_capacity));
    u_strToWCS(
        w_buffer_.data(),
        dest_capacity,
        nullptr,
        icu_str.getBuffer(),
        icu_str.length(),
        &error_code);

    if (error_code != U_ZERO_ERROR)
    {
        DAS_CORE_LOG_ERROR(
            "u_strToWCS failed with error = {}",
            static_cast<int>(error_code));
        w_buffer_.clear();
        return DAS_E_INVALID_STRING;
    }

    w_ready_ = true;
    return DAS_S_OK;
#endif
}

DasResult DasReadOnlyStringProxy::EnsureUtf32Derived()
{
    if (utf32_ready_)
    {
        return DAS_S_OK;
    }

    icu::UnicodeString icu_str(
        reinterpret_cast<const UChar*>(utf16_buffer_.data()),
        static_cast<int32_t>(utf16_buffer_.size()));

    int32_t char_count = icu_str.countChar32();
    utf32_buffer_.resize(static_cast<size_t>(char_count) + 1); // +1 for null
    UErrorCode error_code = U_ZERO_ERROR;
    icu_str.toUTF32(
        reinterpret_cast<UChar32*>(utf32_buffer_.data()),
        char_count,
        error_code);

    if (error_code != U_ZERO_ERROR)
    {
        DAS_CORE_LOG_ERROR(
            "toUTF32 failed with error = {}",
            static_cast<int>(error_code));
        utf32_buffer_.clear();
        return DAS_E_INVALID_STRING;
    }

    utf32_buffer_[static_cast<size_t>(char_count)] = 0; // null terminate
    utf32_ready_ = true;
    return DAS_S_OK;
}

// ── IDasReadOnlyString interface ──────────────────────────────────────────

DasResult DasReadOnlyStringProxy::GetUtf8(const char** out_string)
{
    if (out_string == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    DasResult result = EnsureUtf16Loaded();
    if (DAS::IsFailed(result))
    {
        return result;
    }

    result = EnsureUtf8Derived();
    if (DAS::IsFailed(result))
    {
        return result;
    }

    *out_string = utf8_buffer_.c_str();
    return DAS_S_OK;
}

DasResult DasReadOnlyStringProxy::GetUtf16(
    const char16_t** out_string,
    size_t*          out_string_size) noexcept
{
    if (out_string == nullptr || out_string_size == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    DasResult result = EnsureUtf16Loaded();
    if (DAS::IsFailed(result))
    {
        return result;
    }

    *out_string = utf16_buffer_.data();
    *out_string_size = utf16_buffer_.size();
    return DAS_S_OK;
}

DasResult DasReadOnlyStringProxy::GetW(const wchar_t** out_string)
{
    if (out_string == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    DasResult result = EnsureUtf16Loaded();
    if (DAS::IsFailed(result))
    {
        return result;
    }

    result = EnsureWDerived();
    if (DAS::IsFailed(result))
    {
        return result;
    }

    *out_string = w_buffer_.c_str();
    return DAS_S_OK;
}

const int32_t* DasReadOnlyStringProxy::CBegin()
{
    DasResult result = EnsureUtf16Loaded();
    if (DAS::IsFailed(result))
    {
        return nullptr;
    }

    result = EnsureUtf32Derived();
    if (DAS::IsFailed(result))
    {
        return nullptr;
    }

    return utf32_buffer_.data();
}

const int32_t* DasReadOnlyStringProxy::CEnd()
{
    // EnsureUtf32Derived internally calls EnsureUtf16Loaded
    DasResult result = EnsureUtf32Derived();
    if (DAS::IsFailed(result))
    {
        return nullptr;
    }

    return utf32_buffer_.data() + utf32_buffer_.size();
}

DAS_CORE_IPC_NS_END
