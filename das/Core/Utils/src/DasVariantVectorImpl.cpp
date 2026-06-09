#include "das/DasConfig.h"
#include "das/DasPtr.hpp"
#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/DasVariantVectorImpl.h>
#include <das/DasApi.h>
#include <das/DasSwigApi.h>
#include <das/Utils/CommonUtils.hpp>
#include <limits>

using DasVariantType = Das::ExportInterface::DasVariantType;

DAS_DEFINE_RET_TYPE(DasRetVariantType, DasVariantType);

using Das::ExportInterface::DAS_VARIANT_TYPE_BASE;
using Das::ExportInterface::DAS_VARIANT_TYPE_BOOL;
using Das::ExportInterface::DAS_VARIANT_TYPE_COMPONENT;
using Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT;
using Das::ExportInterface::DAS_VARIANT_TYPE_FORCE_DWORD;
using Das::ExportInterface::DAS_VARIANT_TYPE_IMAGE;
using Das::ExportInterface::DAS_VARIANT_TYPE_INT;
using Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
using Das::ExportInterface::DAS_VARIANT_TYPE_STRING;

DAS_CORE_UTILS_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto MakeSuccess(DasVariantType e) -> DasRetVariantType
{
    return {.error_code = DAS_S_OK, .value = e};
}

template <class T>
DasResult GetVariant(const DasVariantVectorImpl::Variant& v, T* p_out)
{
    const auto* p_value = std::get_if<T>(&v);
    if (p_value == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error: actual variant index = {} does not match "
            "requested type.",
            static_cast<int>(v.index()));
        return DAS_E_TYPE_ERROR;
    }

    *p_out = *p_value;
    return DAS_S_OK;
}

DasRetVariantType VariantToType(const DasVariantVectorImpl::Variant& v)
{
    switch (v.index())
    {
    case 0:
        return Details::MakeSuccess(DAS_VARIANT_TYPE_INT);
    case 1:
        return Details::MakeSuccess(DAS_VARIANT_TYPE_FLOAT);
    case 2:
        return Details::MakeSuccess(DAS_VARIANT_TYPE_BOOL);
    case 3:
        return Details::MakeSuccess(DAS_VARIANT_TYPE_STRING);
    case 4:
        return Details::MakeSuccess(DAS_VARIANT_TYPE_BASE);
    case 5:
        return Details::MakeSuccess(DAS_VARIANT_TYPE_COMPONENT);
    case 6:
        return Details::MakeSuccess(DAS_VARIANT_TYPE_IMAGE);
    case 7:
        return Details::MakeSuccess(DAS_VARIANT_TYPE_NULL);
    default:
        DAS_CORE_LOG_ERROR("Unexpected variant index={}.", v.index());
        return {
            .error_code = DAS_E_INTERNAL_FATAL_ERROR,
            .value = DAS_VARIANT_TYPE_FORCE_DWORD};
    }
}

DAS_NS_ANONYMOUS_DETAILS_END

// ── Int group ──────────────────────────────────────────────────────────────

DasResult DasVariantVectorImpl::GetInt(uint64_t index, int64_t* p_out_int)
{
    DAS_UTILS_CHECK_POINTER(p_out_int);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }
    return Details::GetVariant(variants_[index], p_out_int);
}

DasResult DasVariantVectorImpl::SetInt(uint64_t index, int64_t in_int)
{
    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }
    try
    {
        variants_[index] = in_int;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("Out of memory!");
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasVariantVectorImpl::PushBackInt(int64_t in_int)
{
    try
    {
        variants_.emplace_back(in_int);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ── Float group ────────────────────────────────────────────────────────────

DasResult DasVariantVectorImpl::GetFloat(uint64_t index, float* p_out_float)
{
    DAS_UTILS_CHECK_POINTER(p_out_float);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }
    return Details::GetVariant(variants_[index], p_out_float);
}

DasResult DasVariantVectorImpl::SetFloat(uint64_t index, float in_float)
{
    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }
    try
    {
        variants_[index] = in_float;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("Out of memory!");
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasVariantVectorImpl::PushBackFloat(float in_float)
{
    try
    {
        variants_.emplace_back(in_float);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ── String group ───────────────────────────────────────────────────────────

DasResult DasVariantVectorImpl::GetString(
    uint64_t             index,
    IDasReadOnlyString** pp_out_string)
{
    DAS_UTILS_CHECK_POINTER(pp_out_string);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto* p_value = std::get_if<DasReadOnlyString>(&variants_[index]);
    if (p_value == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error. Expected STRING (index=3), actual index={}.",
            static_cast<int>(variants_[index].index()));
        return DAS_E_TYPE_ERROR;
    }

    p_value->GetImpl(pp_out_string);
    return DAS_S_OK;
}

DasResult DasVariantVectorImpl::SetString(
    uint64_t            index,
    IDasReadOnlyString* in_string)
{
    DAS_UTILS_CHECK_POINTER(in_string);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }
    try
    {
        variants_[index] = DasReadOnlyString{in_string};
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("Out of memory!");
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasVariantVectorImpl::PushBackString(IDasReadOnlyString* in_string)
{
    DAS_UTILS_CHECK_POINTER(in_string);

    try
    {
        variants_.emplace_back(DasReadOnlyString{in_string});
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ── Bool group ─────────────────────────────────────────────────────────────

DasResult DasVariantVectorImpl::GetBool(uint64_t index, bool* p_out_bool)
{
    DAS_UTILS_CHECK_POINTER(p_out_bool);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }
    return Details::GetVariant(variants_[index], p_out_bool);
}

DasResult DasVariantVectorImpl::SetBool(uint64_t index, bool in_bool)
{
    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }
    try
    {
        variants_[index] = in_bool;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("Out of memory!");
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasVariantVectorImpl::PushBackBool(bool in_bool)
{
    try
    {
        variants_.emplace_back(in_bool);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ── Component group ────────────────────────────────────────────────────────

DasResult DasVariantVectorImpl::GetComponent(
    uint64_t                              index,
    Das::PluginInterface::IDasComponent** pp_out_component)
{
    DAS_UTILS_CHECK_POINTER(pp_out_component);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto* p_value =
        std::get_if<Das::PluginInterface::DasComponent>(&variants_[index]);
    if (p_value == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error. Expected COMPONENT (index=5), actual index={}.",
            static_cast<int>(variants_[index].index()));
        return DAS_E_TYPE_ERROR;
    }

    auto* p_raw = p_value->Get();
    if (p_raw != nullptr)
    {
        p_raw->AddRef();
    }
    *pp_out_component = p_raw;
    return DAS_S_OK;
}

DasResult DasVariantVectorImpl::SetComponent(
    uint64_t                             index,
    Das::PluginInterface::IDasComponent* in_component)
{
    DAS_UTILS_CHECK_POINTER(in_component);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }
    try
    {
        variants_[index] = Das::PluginInterface::DasComponent{in_component};
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("Out of memory!");
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasVariantVectorImpl::PushBackComponent(
    Das::PluginInterface::IDasComponent* in_component)
{
    DAS_UTILS_CHECK_POINTER(in_component);

    try
    {
        variants_.emplace_back(
            Das::PluginInterface::DasComponent{in_component});
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ── Base group ─────────────────────────────────────────────────────────────

DasResult DasVariantVectorImpl::GetBase(uint64_t index, IDasBase** pp_out_base)
{
    DAS_UTILS_CHECK_POINTER(pp_out_base);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto* p_value = std::get_if<DasBase>(&variants_[index]);
    if (p_value == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error. Expected BASE (index=4), actual index={}.",
            static_cast<int>(variants_[index].index()));
        return DAS_E_TYPE_ERROR;
    }

    auto* p_raw = p_value->Get();
    if (p_raw != nullptr)
    {
        p_raw->AddRef();
    }
    *pp_out_base = p_raw;
    return DAS_S_OK;
}

DasResult DasVariantVectorImpl::SetBase(uint64_t index, IDasBase* in_base)
{
    DAS_UTILS_CHECK_POINTER(in_base);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }
    try
    {
        variants_[index] = DasBase{in_base};
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("Out of memory!");
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasVariantVectorImpl::PushBackBase(IDasBase* in_base)
{
    DAS_UTILS_CHECK_POINTER(in_base);

    try
    {
        variants_.emplace_back(DasBase{in_base});
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ── Image group ────────────────────────────────────────────────────────────

DasResult DasVariantVectorImpl::GetImage(
    uint64_t                          index,
    Das::ExportInterface::IDasImage** pp_out_image)
{
    DAS_UTILS_CHECK_POINTER(pp_out_image);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto* p_value =
        std::get_if<Das::ExportInterface::DasImage>(&variants_[index]);
    if (p_value == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error. Expected IMAGE (index=6), actual index={}.",
            static_cast<int>(variants_[index].index()));
        return DAS_E_TYPE_ERROR;
    }

    auto* p_raw = p_value->Get();
    if (p_raw != nullptr)
    {
        p_raw->AddRef();
    }
    *pp_out_image = p_raw;
    return DAS_S_OK;
}

DasResult DasVariantVectorImpl::SetImage(
    uint64_t                         index,
    Das::ExportInterface::IDasImage* p_image)
{
    DAS_UTILS_CHECK_POINTER(p_image);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }
    try
    {
        variants_[index] = Das::ExportInterface::DasImage{p_image};
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("Out of memory!");
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasVariantVectorImpl::PushBackImage(
    Das::ExportInterface::IDasImage* p_image)
{
    DAS_UTILS_CHECK_POINTER(p_image);

    try
    {
        variants_.emplace_back(Das::ExportInterface::DasImage{p_image});
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ── Null group ─────────────────────────────────────────────────────────────

DasResult DasVariantVectorImpl::IsNull(uint64_t index, bool* out_is_null)
{
    DAS_UTILS_CHECK_POINTER(out_is_null);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    *out_is_null = std::holds_alternative<std::monostate>(variants_[index]);
    return DAS_S_OK;
}

DasResult DasVariantVectorImpl::PushBackNull()
{
    try
    {
        variants_.emplace_back(std::monostate{});
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ── Utility methods ────────────────────────────────────────────────────────

DasResult DasVariantVectorImpl::GetType(
    uint64_t        index,
    DasVariantType* p_out_type)
{
    DAS_UTILS_CHECK_POINTER(p_out_type);

    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    const auto ret_type = Details::VariantToType(variants_[index]);
    if (DAS::IsOk(ret_type.error_code))
    {
        *p_out_type = ret_type.value;
        return ret_type.error_code;
    }
    return ret_type.error_code;
}

DasResult DasVariantVectorImpl::RemoveAt(uint64_t index)
{
    if (index >= variants_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }
    variants_.erase(variants_.begin() + index);
    return DAS_S_OK;
}

DasResult DasVariantVectorImpl::GetSize()
{
    const auto size = variants_.size();
    if (size > static_cast<size_t>(std::numeric_limits<DasResult>::max()))
    {
        DAS_CORE_LOG_ERROR("Overflow detected! Size = {}.", size);
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
    return static_cast<DasResult>(size);
}

DAS_CORE_UTILS_NS_END

DasResult CreateIDasVariantVector(
    Das::ExportInterface::IDasVariantVector** pp_out_vector)
{
    DAS_UTILS_CHECK_POINTER(pp_out_vector);

    try
    {
        auto* result = new DAS::Core::Utils::DasVariantVectorImpl{};
        result->AddRef();
        *pp_out_vector = result;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasRetIDasVariantVector CreateDasRetIDasVariantVector()
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> p_vector;
    const auto result = CreateIDasVariantVector(p_vector.Put());
    if (DAS::IsFailed(result))
    {
        DasRetIDasVariantVector ret;
        ret.SetErrorCode(result);
        return ret;
    }
    DasRetIDasVariantVector ret;
    ret.SetErrorCode(DAS_S_OK);
    ret.SetValue(p_vector.Get());
    p_vector.Reset();
    return ret;
}
