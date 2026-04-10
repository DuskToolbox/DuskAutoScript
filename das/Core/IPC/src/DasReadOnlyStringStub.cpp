#include <das/Core/IPC/DasReadOnlyStringStub.h>

#include <das/Core/IPC/MemorySerializer.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/DasGuidHolder.h>
#include <das/DasString.hpp>
#include <das/DasTypes.hpp>

DAS_CORE_IPC_NS_BEGIN

DasResult DasReadOnlyStringStub::DispatchMethod(
    uint16_t              method_id,
    void*                 impl,
    const uint8_t*        params,
    size_t                params_size,
    StubContext&          ctx,
    std::vector<uint8_t>& out_response)
{
    (void)params;
    (void)params_size;
    (void)ctx;

    switch (method_id)
    {
    case 0:
        return HandleGetUtf16Snapshot(impl, out_response);
    default:
        return DAS_E_IPC_UNKNOWN_METHOD;
    }
}

DasResult DasReadOnlyStringStub::HandleGetUtf16Snapshot(
    void*                 impl,
    std::vector<uint8_t>& out_response)
{
    auto* readonly_string = static_cast<IDasReadOnlyString*>(impl);

    const char16_t* utf16_data = nullptr;
    size_t          char_count = 0;
    DasResult result = readonly_string->GetUtf16(&utf16_data, &char_count);

    MemorySerializerWriter writer;
    writer.WriteInt32(static_cast<int32_t>(result));
    writer.WriteUInt64(static_cast<uint64_t>(char_count));

    if (DAS::IsOk(result) && char_count > 0 && utf16_data != nullptr)
    {
        writer.Write(utf16_data, char_count * sizeof(char16_t));
    }

    out_response = std::move(writer.GetBuffer());
    return result;
}

DAS_CORE_IPC_NS_END
