#ifndef DAS_CORE_IPC_DAS_VARIANT_VECTOR_BY_VALUE_STUB_H
#define DAS_CORE_IPC_DAS_VARIANT_VECTOR_BY_VALUE_STUB_H

#include <das/Core/IPC/IStubBase.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/DasGuidHolder.h>
#include <das/_autogen/idl/abi/IDasVariantVector.h>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief IDasVariantVector 的 IPC Stub（手动实现，按值序列化）
 *
 * 处理所有 21 个方法（method_id 0-20）：
 * - Get 方法（0-5, 18-20）：统一返回整个 VariantVector 全量快照
 * - Set 方法（6-11）：从 params 反序列化 index+value，写入本地 impl
 * - PushBack 方法（12-17）：从 params 反序列化 value，追加到本地 impl
 *
 * Wire format（Get 响应 - 全量快照）：
 *   [count:8B][type1:4B][value1:varies][type2:4B][value2:varies]...
 *
 * Wire format（Set 请求）：
 *   [index:8B][value:varies]
 *
 * Wire format（PushBack 请求）：
 *   [value:varies]
 *
 * 值类型的 value 大小：
 *   INT    = 8B (int64_t)
 *   FLOAT  = 4B (float)
 *   BOOL   = 1B (uint8_t)
 *   STRING = [str_len:8B][utf8_bytes:str_len]
 *
 * COMPONENT/BASE 的 value（共 12B）：
 *   [session_id:2B][generation:2B][local_id:4B][interface_id:4B]
 */
class DasVariantVectorByValueStub final : public IStubBase
{
public:
    static constexpr uint32_t InterfaceId =
        ComputeInterfaceId(DasIidOf<Das::ExportInterface::IDasVariantVector>());

    [[nodiscard]]
    uint32_t GetInterfaceId() const noexcept override
    {
        return InterfaceId;
    }

    DasResult DispatchMethod(
        uint16_t              method_id,
        void*                 impl,
        const uint8_t*        params,
        size_t                params_size,
        StubContext&          ctx,
        std::vector<uint8_t>& out_response) override;

private:
    /// Get 方法统一处理：返回全量 VariantVector 快照
    DasResult HandleGetSnapshot(
        void*                 impl,
        StubContext&          ctx,
        std::vector<uint8_t>& out_response);

    /// Set 方法：从 params 读取 index + value，调用 impl 的 Set 方法
    DasResult HandleSet(
        uint16_t       method_id,
        void*          impl,
        const uint8_t* params,
        size_t         params_size);

    /// PushBack 方法：从 params 读取 value，调用 impl 的 PushBack 方法
    DasResult HandlePushBack(
        uint16_t       method_id,
        void*          impl,
        const uint8_t* params,
        size_t         params_size);
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_DAS_VARIANT_VECTOR_BY_VALUE_STUB_H
