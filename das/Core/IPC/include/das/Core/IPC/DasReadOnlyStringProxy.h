#ifndef DAS_CORE_IPC_DAS_READ_ONLY_STRING_PROXY_H
#define DAS_CORE_IPC_DAS_READ_ONLY_STRING_PROXY_H

#include <atomic>
#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/DasGuidHolder.h>
#include <das/DasString.hpp>
#include <string>
#include <vector>

DAS_CORE_IPC_NS_BEGIN

class DasReadOnlyStringProxy final : public DasProxyBase<IDasReadOnlyString>,
                                     public IDasReadOnlyString
{
public:
    static constexpr uint32_t InterfaceId =
        ComputeInterfaceId(DasIidOf<IDasReadOnlyString>());

    DasReadOnlyStringProxy(
        uint32_t                      interface_id,
        const ObjectId&               object_id,
        IpcRunLoop&                   run_loop,
        std::weak_ptr<BusinessThread> business_thread,
        ProxyFactory&                 proxy_factory);

    ~DasReadOnlyStringProxy() override = default;

    // IUnknown - 由自身 ref_count_ 管理（不继承 IPCProxyBase 的
    // AddRef/Release）
    uint32_t AddRef() override { return ++ref_count_; }

    uint32_t Release() override
    {
        uint32_t count = --ref_count_;
        if (count == 0)
        {
            proxy_factory_.RemoveFromCache(GetObjectId());
            delete this;
        }
        return count;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override
    {
        if (pp_object == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        if (iid == DAS_IID_READ_ONLY_STRING)
        {
            *pp_object = static_cast<IDasReadOnlyString*>(this);
            AddRef();
            return DAS_S_OK;
        }

        // Try remote QI for other interfaces
        return QueryInterfaceRemote(iid, pp_object);
    }

    // IDasReadOnlyString interface
    DasResult GetUtf8(const char** out_string) override;
    DasResult GetUtf16(
        const char16_t** out_string,
        size_t*          out_string_size) noexcept override;
    DasResult      GetW(const wchar_t** out_string) override;
    const int32_t* CBegin() override;
    const int32_t* CEnd() override;

private:
    /// @brief Fetch UTF-16 snapshot from remote (method_id=0). Idempotent.
    DasResult EnsureUtf16Loaded();

    /// @brief Derive UTF-8 from the cached UTF-16 buffer. Idempotent.
    DasResult EnsureUtf8Derived();

    /// @brief Derive wstring from the cached UTF-16 buffer. Idempotent.
    DasResult EnsureWDerived();

    /// @brief Derive UTF-32 from the cached UTF-16 buffer. Idempotent.
    DasResult EnsureUtf32Derived();

    // Canonical cache — populated by the single RPC
    std::u16string utf16_buffer_;

    // Derived caches — lazy-converted from utf16_buffer_
    std::string          utf8_buffer_;
    std::wstring         w_buffer_;
    std::vector<int32_t> utf32_buffer_;

    bool utf8_ready_ = false;
    bool utf16_ready_ = false;
    bool w_ready_ = false;
    bool utf32_ready_ = false;

    // 引用计数（自身独立管理，不使用 IPCProxyBase 的）
    std::atomic<uint32_t> ref_count_{1};
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_DAS_READ_ONLY_STRING_PROXY_H
