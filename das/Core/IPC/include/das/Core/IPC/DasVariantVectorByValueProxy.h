#ifndef DAS_CORE_IPC_DAS_VARIANT_VECTOR_BY_VALUE_PROXY_H
#define DAS_CORE_IPC_DAS_VARIANT_VECTOR_BY_VALUE_PROXY_H

#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/DasGuidHolder.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/DasTypes.hpp>
#include <string>
#include <variant>
#include <vector>

// Autogen ABI header for IDasVariantVector (provides DasVariantType enum +
// interface)
#include <das/_autogen/idl/abi/IDasVariantVector.h>

DAS_CORE_IPC_NS_BEGIN

/// @brief A single cached variant element deserialized from the remote stub
struct CachedVariant
{
    ::Das::ExportInterface::DasVariantType type =
        ::Das::ExportInterface::DAS_VARIANT_TYPE_INT;

    // Value storage — which member is valid depends on `type`
    int64_t     int_value = 0;
    float       float_value = 0.0f;
    std::string string_value;
    bool        bool_value = false;
    ObjectId    object_id{};    // for BASE and COMPONENT (encoded reference)
    uint32_t    interface_id{}; // interface_id from wire for BASE/COMPONENT
    DAS::DasPtr<IDasBase> base_ptr; // resolved proxy for BASE/COMPONENT
};

/// @brief Handwritten by-value proxy for IDasVariantVector.
///
/// Lazy-loads a full snapshot from the remote stub on first read via
/// REQUEST + BUSINESS_EVENT, then caches locally.
/// Write operations update the local cache and fire-and-forget an
/// EVENT + BUSINESS_EVENT back to the remote stub.
class DasVariantVectorByValueProxy final
    : public DasProxyBase<::Das::ExportInterface::IDasVariantVector>,
      public ::Das::ExportInterface::IDasVariantVector
{
public:
    static constexpr uint32_t InterfaceId =
        ComputeInterfaceId(DasIidOf<Das::ExportInterface::IDasVariantVector>());

    DasVariantVectorByValueProxy(
        uint32_t                      interface_id,
        const ObjectId&               object_id,
        IpcRunLoop&                   run_loop,
        std::weak_ptr<BusinessThread> business_thread,
        ProxyFactory&                 proxy_factory);

    ~DasVariantVectorByValueProxy() override = default;

    uint32_t AddRef() override { return AddRefImpl(); }

    uint32_t Release() override { return ReleaseImpl(); }

    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;

    // ── IDasVariantVector: Int group ────────────────────────────────────
    DasResult GetInt(uint64_t index, int64_t* p_out_int) override;
    DasResult SetInt(uint64_t index, int64_t in_int) override;
    DasResult PushBackInt(int64_t in_int) override;

    // ── IDasVariantVector: Float group ──────────────────────────────────
    DasResult GetFloat(uint64_t index, float* p_out_float) override;
    DasResult SetFloat(uint64_t index, float in_float) override;
    DasResult PushBackFloat(float in_float) override;

    // ── IDasVariantVector: String group ─────────────────────────────────
    DasResult GetString(uint64_t index, IDasReadOnlyString** pp_out_string)
        override;
    DasResult SetString(uint64_t index, IDasReadOnlyString* in_string) override;
    DasResult PushBackString(IDasReadOnlyString* in_string) override;

    // ── IDasVariantVector: Bool group ───────────────────────────────────
    DasResult GetBool(uint64_t index, bool* p_out_bool) override;
    DasResult SetBool(uint64_t index, bool in_bool) override;
    DasResult PushBackBool(bool in_bool) override;

    // ── IDasVariantVector: Component group ──────────────────────────────
    DasResult GetComponent(
        uint64_t                                index,
        ::Das::PluginInterface::IDasComponent** pp_out_component) override;
    DasResult SetComponent(
        uint64_t                               index,
        ::Das::PluginInterface::IDasComponent* in_component) override;
    DasResult PushBackComponent(
        ::Das::PluginInterface::IDasComponent* in_component) override;

    // ── IDasVariantVector: Base group ───────────────────────────────────
    DasResult GetBase(uint64_t index, ::IDasBase** pp_out_base) override;
    DasResult SetBase(uint64_t index, ::IDasBase* in_base) override;
    DasResult PushBackBase(::IDasBase* in_base) override;

    // ── IDasVariantVector: Image group ──────────────────────────────────
    DasResult GetImage(
        uint64_t                          index,
        Das::ExportInterface::IDasImage** pp_out_image) override;
    DasResult SetImage(uint64_t index, Das::ExportInterface::IDasImage* p_image)
        override;
    DasResult PushBackImage(Das::ExportInterface::IDasImage* p_image) override;

    // ── IDasVariantVector: Null group ───────────────────────────────────
    DasResult IsNull(uint64_t index, bool* out_is_null) override;
    DasResult PushBackNull() override;

    // ── IDasVariantVector: Utility methods ──────────────────────────────
    DasResult GetType(
        uint64_t                                index,
        ::Das::ExportInterface::DasVariantType* p_out_type) override;
    DasResult RemoveAt(uint64_t index) override;
    DasResult GetSize() override;

private:
    /// @brief Lazy-load full VariantVector snapshot from remote stub.
    ///        Sends REQUEST + BUSINESS_EVENT, deserializes response into
    ///        cache_.
    DasResult EnsureDataLoaded(uint16_t method_id);

    /// @brief Fire-and-forget a write-back EVENT + BUSINESS_EVENT to remote
    /// stub.
    DasResult SendWriteBack(
        uint16_t       method_id,
        const uint8_t* params,
        size_t         params_size);

    // Local cache
    std::vector<CachedVariant> cache_;
    bool                       data_loaded_ = false;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_DAS_VARIANT_VECTOR_BY_VALUE_PROXY_H
