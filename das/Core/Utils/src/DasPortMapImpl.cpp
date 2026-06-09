#include "das/DasConfig.h"
#include "das/DasPtr.hpp"
#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/DasPortMapImpl.h>
#include <das/DasApi.h>
#include <das/DasSwigApi.h>
#include <das/Utils/CommonUtils.hpp>

using DasVariantType = Das::ExportInterface::DasVariantType;

using Das::ExportInterface::DAS_VARIANT_TYPE_BASE;
using Das::ExportInterface::DAS_VARIANT_TYPE_BOOL;
using Das::ExportInterface::DAS_VARIANT_TYPE_COMPONENT;
using Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT;
using Das::ExportInterface::DAS_VARIANT_TYPE_IMAGE;
using Das::ExportInterface::DAS_VARIANT_TYPE_INT;
using Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
using Das::ExportInterface::DAS_VARIANT_TYPE_STRING;

DAS_CORE_UTILS_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

std::string PortIdToString(IDasReadOnlyString* p_port_id)
{
    if (p_port_id == nullptr)
    {
        return {};
    }
    const char* raw = nullptr;
    p_port_id->GetUtf8(&raw);
    return raw ? std::string{raw} : std::string{};
}

// NOTE: DasPortMapImpl::Variant and DasVariantVectorImpl::Variant are
// independent types with different index orders for BASE/COMPONENT/IMAGE.
// Do NOT assume they are interchangeable — always use VariantToType()
// to resolve the correct DasVariantType for each variant type.
DasVariantType VariantToType(const DasPortMapImpl::Variant& v)
{
    switch (v.index())
    {
    case 0:
        return DAS_VARIANT_TYPE_INT;
    case 1:
        return DAS_VARIANT_TYPE_FLOAT;
    case 2:
        return DAS_VARIANT_TYPE_BOOL;
    case 3:
        return DAS_VARIANT_TYPE_STRING;
    case 4:
        return DAS_VARIANT_TYPE_IMAGE;
    case 5:
        return DAS_VARIANT_TYPE_BASE;
    case 6:
        return DAS_VARIANT_TYPE_COMPONENT;
    case 7:
        return DAS_VARIANT_TYPE_NULL;
    default:
        DAS_CORE_LOG_ERROR("Unexpected variant index = {}.", v.index());
        return DAS_VARIANT_TYPE_NULL;
    }
}

DAS_NS_ANONYMOUS_DETAILS_END

// ── Read-only methods ───────────────────────────────────────────────────

DasResult DasPortMapImpl::Has(IDasReadOnlyString* p_port_id, bool* out_has)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(out_has);

    const auto key = Details::PortIdToString(p_port_id);
    *out_has = entries_.contains(key);
    return DAS_S_OK;
}

DasResult DasPortMapImpl::GetType(
    IDasReadOnlyString* p_port_id,
    DasVariantType*     out_kind)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(out_kind);

    const auto key = Details::PortIdToString(p_port_id);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        DAS_CORE_LOG_ERROR("Port not found: key = {}.", key);
        return DAS_E_OUT_OF_RANGE;
    }

    *out_kind = Details::VariantToType(it->second);
    return DAS_S_OK;
}

DasResult DasPortMapImpl::GetBool(
    IDasReadOnlyString* p_port_id,
    bool*               out_value)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(out_value);

    const auto key = Details::PortIdToString(p_port_id);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto* p_val = std::get_if<bool>(&it->second);
    if (p_val == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error. Expected BOOL, actual index = {}.",
            static_cast<int>(it->second.index()));
        return DAS_E_TYPE_ERROR;
    }
    *out_value = *p_val;
    return DAS_S_OK;
}

DasResult DasPortMapImpl::GetInt(
    IDasReadOnlyString* p_port_id,
    int64_t*            out_value)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(out_value);

    const auto key = Details::PortIdToString(p_port_id);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto* p_val = std::get_if<int64_t>(&it->second);
    if (p_val == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error. Expected INT, actual index = {}.",
            static_cast<int>(it->second.index()));
        return DAS_E_TYPE_ERROR;
    }
    *out_value = *p_val;
    return DAS_S_OK;
}

DasResult DasPortMapImpl::GetFloat(
    IDasReadOnlyString* p_port_id,
    double*             out_value)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(out_value);

    const auto key = Details::PortIdToString(p_port_id);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto* p_val = std::get_if<double>(&it->second);
    if (p_val == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error. Expected FLOAT, actual index = {}.",
            static_cast<int>(it->second.index()));
        return DAS_E_TYPE_ERROR;
    }
    *out_value = *p_val;
    return DAS_S_OK;
}

DasResult DasPortMapImpl::GetString(
    IDasReadOnlyString*  p_port_id,
    IDasReadOnlyString** pp_out_string)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(pp_out_string);

    const auto key = Details::PortIdToString(p_port_id);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto* p_val = std::get_if<DasReadOnlyString>(&it->second);
    if (p_val == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error. Expected STRING, actual index = {}.",
            static_cast<int>(it->second.index()));
        return DAS_E_TYPE_ERROR;
    }

    p_val->GetImpl(pp_out_string);
    return DAS_S_OK;
}

DasResult DasPortMapImpl::GetJson(
    IDasReadOnlyString*  p_port_id,
    IDasReadOnlyString** pp_out_json)
{
    return GetString(p_port_id, pp_out_json);
}

DasResult DasPortMapImpl::GetImage(
    IDasReadOnlyString*               p_port_id,
    Das::ExportInterface::IDasImage** pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(pp_out_image);

    const auto key = Details::PortIdToString(p_port_id);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto* p_val =
        std::get_if<Das::ExportInterface::DasImage>(&it->second);
    if (p_val == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error. Expected IMAGE, actual index = {}.",
            static_cast<int>(it->second.index()));
        return DAS_E_TYPE_ERROR;
    }

    auto* p_raw = p_val->Get();
    if (p_raw != nullptr)
    {
        p_raw->AddRef();
    }
    *pp_out_image = p_raw;
    return DAS_S_OK;
}

DasResult DasPortMapImpl::GetBase(
    IDasReadOnlyString* p_port_id,
    DasGuid             iid,
    IDasBase**          pp_out_object)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(pp_out_object);

    const auto key = Details::PortIdToString(p_port_id);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto* p_val = std::get_if<DasBase>(&it->second);
    if (p_val == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error. Expected BASE, actual index = {}.",
            static_cast<int>(it->second.index()));
        return DAS_E_TYPE_ERROR;
    }

    auto*     p_raw = p_val->Get();
    DasResult result = DAS_S_OK;
    if (p_raw != nullptr)
    {
        result =
            p_raw->QueryInterface(iid, reinterpret_cast<void**>(pp_out_object));
    }
    else
    {
        *pp_out_object = nullptr;
    }
    return result;
}

DasResult DasPortMapImpl::GetComponent(
    IDasReadOnlyString* p_port_id,
    DasGuid             iid,
    IDasBase**          pp_out_component)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(pp_out_component);

    const auto key = Details::PortIdToString(p_port_id);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto* p_val =
        std::get_if<Das::PluginInterface::DasComponent>(&it->second);
    if (p_val == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error. Expected COMPONENT, actual index = {}.",
            static_cast<int>(it->second.index()));
        return DAS_E_TYPE_ERROR;
    }

    auto*     p_raw = p_val->Get();
    DasResult result = DAS_S_OK;
    if (p_raw != nullptr)
    {
        result = p_raw->QueryInterface(
            iid,
            reinterpret_cast<void**>(pp_out_component));
    }
    else
    {
        *pp_out_component = nullptr;
    }
    return result;
}

DasResult DasPortMapImpl::GetKeys(
    Das::ExportInterface::IDasStringVector** pp_out_keys)
{
    DAS_UTILS_CHECK_POINTER(pp_out_keys);

    DAS::DasPtr<Das::ExportInterface::IDasStringVector> p_keys;
    DasResult result = CreateIDasStringVector(p_keys.Put());
    if (DAS::IsFailed(result))
    {
        return result;
    }

    for (const auto& [key, _] : entries_)
    {
        DasReadOnlyString dos{key.c_str()};
        result = p_keys->PushBack(dos.Get());
        if (DAS::IsFailed(result))
        {
            return result;
        }
    }

    *pp_out_keys = p_keys.Get();
    p_keys.Get()->AddRef();
    return DAS_S_OK;
}

// ── Writable methods ────────────────────────────────────────────────────

DasResult DasPortMapImpl::SetInt(
    IDasReadOnlyString* p_port_id,
    int64_t             in_value)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);

    const auto key = Details::PortIdToString(p_port_id);
    try
    {
        entries_[key] = in_value;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasPortMapImpl::SetFloat(
    IDasReadOnlyString* p_port_id,
    double              in_value)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);

    const auto key = Details::PortIdToString(p_port_id);
    try
    {
        entries_[key] = in_value;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasPortMapImpl::SetString(
    IDasReadOnlyString* p_port_id,
    IDasReadOnlyString* in_value)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(in_value);

    const auto key = Details::PortIdToString(p_port_id);
    try
    {
        entries_[key] = DasReadOnlyString{in_value};
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasPortMapImpl::SetBool(IDasReadOnlyString* p_port_id, bool in_value)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);

    const auto key = Details::PortIdToString(p_port_id);
    try
    {
        entries_[key] = in_value;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasPortMapImpl::SetImage(
    IDasReadOnlyString*              p_port_id,
    Das::ExportInterface::IDasImage* p_image)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(p_image);

    const auto key = Details::PortIdToString(p_port_id);
    try
    {
        entries_[key] = Das::ExportInterface::DasImage{p_image};
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasPortMapImpl::SetBase(
    IDasReadOnlyString*      p_port_id,
    [[maybe_unused]] DasGuid iid,
    IDasBase*                p_in_object)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(p_in_object);

    const auto key = Details::PortIdToString(p_port_id);
    try
    {
        entries_[key] = DasBase{p_in_object};
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasPortMapImpl::SetComponent(
    IDasReadOnlyString*      p_port_id,
    [[maybe_unused]] DasGuid iid,
    IDasBase*                p_in_component)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);
    DAS_UTILS_CHECK_POINTER(p_in_component);

    const auto key = Details::PortIdToString(p_port_id);
    try
    {
        // Use QueryInterface to safely verify the object supports
        // IDasComponent before storing it.
        DAS::DasPtr<Das::PluginInterface::IDasComponent> comp;
        DasResult hr = p_in_component->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasComponent>(),
            comp.PutVoid());
        if (DAS::IsFailed(hr))
        {
            DAS_CORE_LOG_ERROR(
                "SetComponent: QueryInterface for IDasComponent failed. hr = {}.",
                static_cast<int>(hr));
            return DAS_E_NO_INTERFACE;
        }
        entries_[key] = Das::PluginInterface::DasComponent{comp.Get()};
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasPortMapImpl::Clear()
{
    entries_.clear();
    return DAS_S_OK;
}

DasResult DasPortMapImpl::Remove(IDasReadOnlyString* p_port_id)
{
    DAS_UTILS_CHECK_POINTER(p_port_id);

    const auto key = Details::PortIdToString(p_port_id);
    const auto erased = entries_.erase(key);
    if (erased == 0)
    {
        DAS_CORE_LOG_ERROR("Port not found for removal: key = {}.", key);
        return DAS_E_OUT_OF_RANGE;
    }
    return DAS_S_OK;
}

DAS_CORE_UTILS_NS_END

DasResult CreateIDasPortMap(Das::ExportInterface::IDasPortMap** pp_out_port_map)
{
    DAS_UTILS_CHECK_POINTER(pp_out_port_map);

    try
    {
        auto* result = new DAS::Core::Utils::DasPortMapImpl{};
        result->AddRef();
        *pp_out_port_map = result;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasRetIDasPortMap CreateDasRetIDasPortMap()
{
    DAS::DasPtr<Das::ExportInterface::IDasPortMap> p_port_map;
    const auto result = CreateIDasPortMap(p_port_map.Put());
    if (DAS::IsFailed(result))
    {
        DasRetIDasPortMap ret;
        ret.SetErrorCode(result);
        return ret;
    }
    DasRetIDasPortMap ret;
    ret.SetErrorCode(DAS_S_OK);
    ret.SetValue(p_port_map.Get());
    p_port_map.Reset();
    return ret;
}
