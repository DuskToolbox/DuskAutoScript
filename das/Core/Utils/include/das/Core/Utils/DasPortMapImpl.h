#ifndef DAS_CORE_UTILS_DASPORTMAPIMPL_H
#define DAS_CORE_UTILS_DASPORTMAPIMPL_H

#include <das/Core/Utils/Config.h>
#include <das/DasBase.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasImage.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasPortMap.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasComponent.hpp>
#include <map>
#include <variant>

DAS_CORE_UTILS_NS_BEGIN

class DasPortMapImpl final
    : public Das::ExportInterface::DasPortMapImplBase<DasPortMapImpl>
{
public:
    using Variant = std::variant<
        int64_t,
        double,
        bool,
        DasReadOnlyString,
        Das::ExportInterface::DasImage,
        DasBase,
        Das::PluginInterface::DasComponent,
        std::monostate>;

    DasPortMapImpl() = default;

    // ── Read-only methods (inherited from IDasReadOnlyPortMap) ─────────
    DAS_IMPL Has(IDasReadOnlyString* p_port_id, bool* out_has) override;
    DAS_IMPL
    GetType(
        IDasReadOnlyString*                   p_port_id,
        Das::ExportInterface::DasVariantType* out_kind) override;
    DAS_IMPL GetBool(IDasReadOnlyString* p_port_id, bool* out_value) override;
    DAS_IMPL
    GetInt(IDasReadOnlyString* p_port_id, int64_t* out_value) override;
    DAS_IMPL
    GetFloat(IDasReadOnlyString* p_port_id, double* out_value) override;
    DAS_IMPL
    GetString(IDasReadOnlyString* p_port_id, IDasReadOnlyString** pp_out_string)
        override;
    DAS_IMPL
    GetJson(IDasReadOnlyString* p_port_id, IDasReadOnlyString** pp_out_json)
        override;
    DAS_IMPL
    GetImage(
        IDasReadOnlyString*               p_port_id,
        Das::ExportInterface::IDasImage** pp_out_image) override;
    DAS_IMPL
    GetBase(
        IDasReadOnlyString* p_port_id,
        DasGuid             iid,
        IDasBase**          pp_out_object) override;
    DAS_IMPL
    GetComponent(
        IDasReadOnlyString* p_port_id,
        DasGuid             iid,
        IDasBase**          pp_out_component) override;
    DAS_IMPL
    GetKeys(Das::ExportInterface::IDasStringVector** pp_out_keys) override;

    // ── Writable methods (IDasPortMap) ─────────────────────────────────
    DAS_IMPL
    SetInt(IDasReadOnlyString* p_port_id, int64_t in_value) override;
    DAS_IMPL
    SetFloat(IDasReadOnlyString* p_port_id, double in_value) override;
    DAS_IMPL SetString(
        IDasReadOnlyString* p_port_id,
        IDasReadOnlyString* in_value) override;
    DAS_IMPL SetBool(IDasReadOnlyString* p_port_id, bool in_value) override;
    DAS_IMPL
    SetImage(
        IDasReadOnlyString*              p_port_id,
        Das::ExportInterface::IDasImage* p_image) override;
    DAS_IMPL
    SetBase(IDasReadOnlyString* p_port_id, DasGuid iid, IDasBase* p_in_object)
        override;
    DAS_IMPL
    SetComponent(
        IDasReadOnlyString* p_port_id,
        DasGuid             iid,
        IDasBase*           p_in_component) override;
    DAS_IMPL Clear() override;
    DAS_IMPL Remove(IDasReadOnlyString* p_port_id) override;

private:
    std::map<std::string, Variant> entries_{};
};

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_DASPORTMAPIMPL_H
