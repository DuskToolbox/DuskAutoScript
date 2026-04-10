#ifndef DAS_CORE_IPC_DAS_READ_ONLY_STRING_STUB_H
#define DAS_CORE_IPC_DAS_READ_ONLY_STRING_STUB_H

#include <das/Core/IPC/IStubBase.h>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief IDasReadOnlyString 的 IPC Stub（手动实现，非 IDL 生成）
 *
 * 处理 method_id=0 (GetUtf16Snapshot)，
 * 返回 [result:4B][char_count:8B][utf16_raw_bytes:char_count*2]。
 * 状态无 → 全局单例，永不销毁。
 */
class DasReadOnlyStringStub final : public IStubBase
{
public:
    static uint32_t InterfaceId;

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
    DasResult HandleGetUtf16Snapshot(
        void*                 impl,
        std::vector<uint8_t>& out_response);
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_DAS_READ_ONLY_STRING_STUB_H
