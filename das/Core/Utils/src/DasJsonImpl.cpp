#include <das/Core/Utils/DasJsonImpl.h>

#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasExport.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/Expected.h>
#include <yyjson.h>

using Das::ExportInterface::DAS_TYPE_BOOL;
using Das::ExportInterface::DAS_TYPE_FLOAT;
using Das::ExportInterface::DAS_TYPE_INT;
using Das::ExportInterface::DAS_TYPE_JSON_ARRAY;
using Das::ExportInterface::DAS_TYPE_JSON_OBJECT;
using Das::ExportInterface::DAS_TYPE_NULL;
using Das::ExportInterface::DAS_TYPE_STRING;
using Das::ExportInterface::DAS_TYPE_UINT;
using Das::ExportInterface::DAS_TYPE_UNSUPPORTED;
using Das::ExportInterface::DasType;
using Das::Utils::ParseYyjsonFromString;

DAS_CORE_UTILS_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

/// Helper: access a named sub-value from an Object or Ref variant.
/// Returns a const_value_ref if found, or nullopt if the key does not
/// exist. Throws DasJsonImplRefExpiredException if Ref is expired.
std::optional<yyjson::writer::const_value_ref> GetValueByName(
    std::variant<IDasJsonImpl::Object, IDasJsonImpl::Ref>& impl,
    std::string_view                                       key)
{
    return std::visit(
        Utils::overload_set{
            [key](IDasJsonImpl::Object& obj)
                -> std::optional<yyjson::writer::const_value_ref>
            {
                auto obj_opt = obj.value_.as_object();
                if (!obj_opt)
                {
                    return std::nullopt;
                }
                const auto& object_ref = obj_opt.value();
                auto        it = object_ref.find(key);
                if (it == object_ref.end())
                {
                    return std::nullopt;
                }
                return it->second;
            },
            [key](IDasJsonImpl::Ref& ref)
                -> std::optional<yyjson::writer::const_value_ref>
            {
                if (ref.val_ == nullptr)
                {
                    throw DasJsonImplRefExpiredException{};
                }
                auto obj_opt = ref.val_->as_object();
                if (!obj_opt)
                {
                    return std::nullopt;
                }
                const auto& object_ref = obj_opt.value();
                auto        it = object_ref.find(key);
                if (it == object_ref.end())
                {
                    return std::nullopt;
                }
                return it->second;
            }},
        impl);
}

/// Helper: access a sub-value by index from an Object or Ref variant.
/// Returns a const_value_ref if found, or nullopt if the index is out of
/// range. Throws DasJsonImplRefExpiredException if Ref is expired. Throws
/// DasJsonImplJsonIsNotArray if value is not an array.
std::optional<yyjson::writer::const_value_ref> GetValueByIndex(
    std::variant<IDasJsonImpl::Object, IDasJsonImpl::Ref>& impl,
    size_t                                                 index)
{
    return std::visit(
        Utils::overload_set{
            [index](IDasJsonImpl::Object& obj)
                -> std::optional<yyjson::writer::const_value_ref>
            {
                auto arr_opt = obj.value_.as_array();
                if (!arr_opt)
                {
                    throw DasJsonImplJsonIsNotArray{};
                }
                const auto& arr = arr_opt.value();
                if (index >= arr.size())
                {
                    return std::nullopt;
                }
                return arr[index];
            },
            [index](IDasJsonImpl::Ref& ref)
                -> std::optional<yyjson::writer::const_value_ref>
            {
                if (ref.val_ == nullptr)
                {
                    throw DasJsonImplRefExpiredException{};
                }
                auto arr_opt = ref.val_->as_array();
                if (!arr_opt)
                {
                    throw DasJsonImplJsonIsNotArray{};
                }
                const auto& arr = arr_opt.value();
                if (index >= arr.size())
                {
                    return std::nullopt;
                }
                return arr[index];
            }},
        impl);
}

/// Helper: ensure the value is an object (for SetByName operations).
/// Returns a mutable object_ref or nullopt on failure.
/// Throws DasJsonImplRefExpiredException if Ref is expired.
std::optional<yyjson::writer::object_ref> GetObjectForSet(
    std::variant<IDasJsonImpl::Object, IDasJsonImpl::Ref>& impl)
{
    return std::visit(
        Utils::overload_set{
            [](IDasJsonImpl::Object& obj)
                -> std::optional<yyjson::writer::object_ref>
            { return obj.value_.as_object(); },
            [](IDasJsonImpl::Ref& ref)
                -> std::optional<yyjson::writer::object_ref>
            {
                if (ref.val_ == nullptr)
                {
                    throw DasJsonImplRefExpiredException{};
                }
                return ref.val_->as_object();
            }},
        impl);
}

/// Helper: ensure the value is an array (for SetByIndex operations).
/// Returns a mutable array_ref or nullopt on failure.
/// Throws DasJsonImplRefExpiredException if Ref is expired.
std::optional<yyjson::writer::array_ref> GetArrayForSet(
    std::variant<IDasJsonImpl::Object, IDasJsonImpl::Ref>& impl)
{
    return std::visit(
        Utils::overload_set{
            [](IDasJsonImpl::Object& obj)
                -> std::optional<yyjson::writer::array_ref>
            { return obj.value_.as_array(); },
            [](IDasJsonImpl::Ref& ref)
                -> std::optional<yyjson::writer::array_ref>
            {
                if (ref.val_ == nullptr)
                {
                    throw DasJsonImplRefExpiredException{};
                }
                return ref.val_->as_array();
            }},
        impl);
}

/// Helper: Creates an owning copy of a const_value_ref as a new value.
yyjson::value CopyValue(const yyjson::writer::const_value_ref& src)
{
    yyjson::value result;
    result = src;
    return result;
}

DAS_NS_ANONYMOUS_DETAILS_END

// ========================================================================
// GetToImpl — ByName (string key)
// ========================================================================

template <class T>
DasResult IDasJsonImpl::GetToImpl(IDasReadOnlyString* p_string, T* obj)
{
    DAS_UTILS_CHECK_POINTER(p_string)
    DAS_UTILS_CHECK_POINTER(obj)

    const auto expected_u8_key = ToU8StringWithoutOwnership(p_string);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_u8_key = expected_u8_key.value();

    std::scoped_lock _{mutex_};
    try
    {
        auto val_opt = Details::GetValueByName(impl_, p_u8_key);
        if (!val_opt)
        {
            return DAS_E_NOT_FOUND;
        }
        const auto& val = val_opt.value();

        if constexpr (std::is_same_v<T, int64_t>)
        {
            auto opt = val.as_sint();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *obj = opt.value();
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            auto opt = val.as_real();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *obj = static_cast<float>(opt.value());
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            auto opt = val.as_bool();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *obj = opt.value();
            return DAS_S_OK;
        }
        else
        {
            return DAS_E_INVALID_JSON;
        }
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
}

// ========================================================================
// GetToImpl — ByName (string key, string out)
// ========================================================================

DasResult IDasJsonImpl::GetToImpl(
    IDasReadOnlyString*  p_string,
    IDasReadOnlyString** obj)
{
    DAS_UTILS_CHECK_POINTER(p_string)
    DAS_UTILS_CHECK_POINTER(obj)

    const auto expected_u8_key = ToU8StringWithoutOwnership(p_string);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_u8_key = expected_u8_key.value();

    std::scoped_lock _{mutex_};
    try
    {
        auto val_opt = Details::GetValueByName(impl_, p_u8_key);
        if (!val_opt)
        {
            return DAS_E_NOT_FOUND;
        }
        const auto& val = val_opt.value();
        auto        opt = val.as_string();
        if (!opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        std::string str_val(opt.value());
        return ::CreateIDasReadOnlyStringFromUtf8(str_val.c_str(), obj);
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_ERROR("Note: key = {}", p_u8_key);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
}

// ========================================================================
// GetToImpl — ByIndex
// ========================================================================

template <class T>
DasResult IDasJsonImpl::GetToImpl(size_t index, T* obj)
{
    DAS_UTILS_CHECK_POINTER(obj)

    std::scoped_lock _{mutex_};
    try
    {
        auto val_opt = Details::GetValueByIndex(impl_, index);
        if (!val_opt)
        {
            return DAS_E_OUT_OF_RANGE;
        }
        const auto& val = val_opt.value();

        if constexpr (std::is_same_v<T, int64_t>)
        {
            auto opt = val.as_sint();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *obj = opt.value();
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            auto opt = val.as_real();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *obj = static_cast<float>(opt.value());
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            auto opt = val.as_bool();
            if (!opt)
            {
                return DAS_E_TYPE_ERROR;
            }
            *obj = opt.value();
            return DAS_S_OK;
        }
        else
        {
            return DAS_E_INVALID_JSON;
        }
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const DasJsonImplJsonIsNotArray& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_TYPE_ERROR;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
}

// ========================================================================
// GetToImpl — ByIndex (string out)
// ========================================================================

DasResult IDasJsonImpl::GetToImpl(size_t index, IDasReadOnlyString** pp_out_obj)
{
    DAS_UTILS_CHECK_POINTER(pp_out_obj)

    std::scoped_lock _{mutex_};
    try
    {
        auto val_opt = Details::GetValueByIndex(impl_, index);
        if (!val_opt)
        {
            return DAS_E_OUT_OF_RANGE;
        }
        const auto& val = val_opt.value();
        auto        opt = val.as_string();
        if (!opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        std::string str_val(opt.value());
        return ::CreateIDasReadOnlyStringFromUtf8(str_val.c_str(), pp_out_obj);
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_ERROR("Note: index = {}", index);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const DasJsonImplJsonIsNotArray& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_TYPE_ERROR;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
}

// ========================================================================
// SetImpl — ByName (string key)
// ========================================================================

template <class T>
DasResult IDasJsonImpl::SetImpl(IDasReadOnlyString* p_string, const T& value)
{
    DAS_UTILS_CHECK_POINTER(p_string)

    const auto expected_u8_key = ToU8StringWithoutOwnership(p_string);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_u8_key = expected_u8_key.value();

    std::scoped_lock _{mutex_};
    try
    {
        auto obj_opt = Details::GetObjectForSet(impl_);
        if (!obj_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        auto& obj = obj_opt.value();

        if constexpr (std::is_same_v<T, int64_t>)
        {
            obj[p_u8_key] = value;
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            obj[p_u8_key] = value;
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            obj[p_u8_key] = value;
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, yyjson::value>)
        {
            obj[p_u8_key] = value;
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, yyjson::writer::const_value_ref>)
        {
            obj[p_u8_key] = value;
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            obj[p_u8_key] = value;
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, const char*>)
        {
            obj[p_u8_key] = std::string(value);
            return DAS_S_OK;
        }
        else
        {
            return DAS_E_INVALID_JSON;
        }
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_ERROR("Note: key = {}", p_u8_key);
        return DAS_E_INVALID_JSON;
    }
}

// ========================================================================
// SetImpl — ByIndex
// ========================================================================

template <class T>
DasResult IDasJsonImpl::SetImpl(size_t index, const T& value)
{
    std::scoped_lock _{mutex_};
    try
    {
        auto arr_opt = Details::GetArrayForSet(impl_);
        if (!arr_opt)
        {
            return DAS_E_TYPE_ERROR;
        }
        auto& arr = arr_opt.value();
        if (index >= arr.size())
        {
            return DAS_E_OUT_OF_RANGE;
        }

        if constexpr (std::is_same_v<T, int64_t>)
        {
            arr[index] = value;
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            arr[index] = value;
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            arr[index] = value;
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, std::string>)
        {
            arr[index] = value;
            return DAS_S_OK;
        }
        else if constexpr (std::is_same_v<T, yyjson::value>)
        {
            arr[index] = value;
            return DAS_S_OK;
        }
        else
        {
            return DAS_E_INVALID_JSON;
        }
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
}

// ========================================================================
// Constructors / Destructor
// ========================================================================

IDasJsonImpl::IDasJsonImpl() : impl_{Object{}} {}

IDasJsonImpl::IDasJsonImpl(yyjson::value& ref_value)
    : impl_{Ref{&ref_value, {}}}
{
}

IDasJsonImpl::IDasJsonImpl(yyjson::value&& value)
    : impl_{Object{std::move(value), {}}}
{
}

IDasJsonImpl::~IDasJsonImpl() {}

IDasJsonImpl::IDasJsonImpl(const char* p_json_string) : impl_{Object{{}, {}}}
{
    auto parsed = ParseYyjsonFromString(p_json_string);
    if (!parsed)
    {
        throw std::runtime_error("JSON parse failed");
    }
    impl_.emplace<Object>(std::move(parsed.value()));
}

// ========================================================================
// QueryInterface
// ========================================================================

DasResult IDasJsonImpl::QueryInterface(const DasGuid& iid, void** pp_out_object)
{
    if (pp_out_object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 检查IID_IDasJson
    if (iid == DasIidOf<ExportInterface::IDasJson>())
    {
        *pp_out_object = static_cast<ExportInterface::IDasJson*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // 检查IID_IDasJsonImpl
    if (iid == DasIidOf<IDasJsonImpl>())
    {
        *pp_out_object = static_cast<IDasJsonImpl*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    // 检查IID_IDasBase
    if (iid == DAS_IID_BASE)
    {
        *pp_out_object = static_cast<IDasBase*>(this);
        this->AddRef();
        return DAS_S_OK;
    }

    *pp_out_object = nullptr;
    return DAS_E_NO_INTERFACE;
}

// ========================================================================
// Get*ByName — delegates to GetToImpl
// ========================================================================

DasResult IDasJsonImpl::GetIntByName(
    IDasReadOnlyString* key,
    int64_t*            p_out_int)
{
    return GetToImpl(key, p_out_int);
}

DasResult IDasJsonImpl::GetFloatByName(
    IDasReadOnlyString* key,
    float*              p_out_float)
{
    return GetToImpl(key, p_out_float);
}

DasResult IDasJsonImpl::GetStringByName(
    IDasReadOnlyString*  key,
    IDasReadOnlyString** pp_out_string)
{
    return GetToImpl(key, pp_out_string);
}

DasResult IDasJsonImpl::GetBoolByName(IDasReadOnlyString* key, bool* p_out_bool)
{
    return GetToImpl(key, p_out_bool);
}

DasResult IDasJsonImpl::GetObjectRefByName(
    IDasReadOnlyString* key,
    IDasJson**          pp_out_das_json)
{
    DAS_UTILS_CHECK_POINTER(key)
    DAS_UTILS_CHECK_POINTER(pp_out_das_json)

    const auto expected_u8_key = ToU8StringWithoutOwnership(key);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_u8_key = expected_u8_key.value();

    std::scoped_lock _{mutex_};
    try
    {
        auto val_opt = Details::GetValueByName(impl_, p_u8_key);
        if (!val_opt)
        {
            return DAS_E_NOT_FOUND;
        }
        const auto& sub_val = val_opt.value();
        if (!sub_val.is_object() && !sub_val.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }

        // Create an owning copy (returns Object, not Ref)
        auto  new_val = Details::CopyValue(sub_val);
        auto* p_result = new IDasJsonImpl(std::move(new_val));
        p_result->AddRef();
        *pp_out_das_json = p_result;
        return DAS_S_OK;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
}

// ========================================================================
// Set*ByName
// ========================================================================

DasResult IDasJsonImpl::SetIntByName(IDasReadOnlyString* key, int64_t in_int)
{
    return SetImpl(key, in_int);
}

DasResult IDasJsonImpl::SetFloatByName(IDasReadOnlyString* key, float in_float)
{
    return SetImpl(key, in_float);
}

DasResult IDasJsonImpl::SetStringByName(
    IDasReadOnlyString* key,
    IDasReadOnlyString* p_in_string)
{
    DAS_UTILS_CHECK_POINTER(p_in_string)

    const auto expected_u8_value = ToU8StringWithoutOwnership(p_in_string);
    if (!expected_u8_value)
    {
        return expected_u8_value.error();
    }
    const auto p_in_u8_value = expected_u8_value.value();

    return SetImpl(key, std::string(p_in_u8_value));
}

DasResult IDasJsonImpl::SetBoolByName(IDasReadOnlyString* key, bool in_bool)
{
    return SetImpl(key, in_bool);
}

DasResult IDasJsonImpl::SetObjectByName(
    IDasReadOnlyString* key,
    IDasJson*           p_in_das_json)
{
    DasPtr<IDasJsonImpl> p_impl;
    if (const auto qi_result = p_in_das_json->QueryInterface(
            DasIidOf<IDasJsonImpl>(),
            p_impl.PutVoid());
        DAS::IsFailed(qi_result))
    {
        return qi_result;
    }

    const auto in_json = std::visit(
        Utils::overload_set{
            [](IDasJsonImpl::Object& j) -> yyjson::value* { return &j.value_; },
            [](IDasJsonImpl::Ref& j) -> yyjson::value*
            {
                if (j.val_ == nullptr)
                {
                    return nullptr;
                }
                return j.val_;
            }},
        p_impl->impl_);

    if (in_json == nullptr)
    {
        return DAS_E_DANGLING_REFERENCE;
    }

    return SetImpl(key, *in_json);
}

// ========================================================================
// Get*ByIndex — delegates to GetToImpl
// ========================================================================

DasResult IDasJsonImpl::GetIntByIndex(size_t index, int64_t* p_out_int)
{
    return GetToImpl(index, p_out_int);
}

DasResult IDasJsonImpl::GetFloatByIndex(size_t index, float* p_out_float)
{
    return GetToImpl(index, p_out_float);
}

DasResult IDasJsonImpl::GetStringByIndex(
    size_t               index,
    IDasReadOnlyString** pp_out_string)
{
    return GetToImpl(index, pp_out_string);
}

DasResult IDasJsonImpl::GetBoolByIndex(size_t index, bool* p_out_bool)
{
    return GetToImpl(index, p_out_bool);
}

DasResult IDasJsonImpl::GetObjectRefByIndex(
    size_t     index,
    IDasJson** pp_out_das_json)
{
    DAS_UTILS_CHECK_POINTER(pp_out_das_json)

    std::scoped_lock _{mutex_};
    try
    {
        auto val_opt = Details::GetValueByIndex(impl_, index);
        if (!val_opt)
        {
            return DAS_E_OUT_OF_RANGE;
        }
        const auto& sub_val = val_opt.value();
        if (!sub_val.is_object() && !sub_val.is_array())
        {
            return DAS_E_TYPE_ERROR;
        }

        // Create an owning copy (returns Object, not Ref)
        auto  new_val = Details::CopyValue(sub_val);
        auto* p_result = new IDasJsonImpl(std::move(new_val));
        p_result->AddRef();
        *pp_out_das_json = p_result;
        return DAS_S_OK;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const DasJsonImplJsonIsNotArray& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_TYPE_ERROR;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
}

// ========================================================================
// Set*ByIndex
// ========================================================================

DasResult IDasJsonImpl::SetIntByIndex(size_t index, int64_t in_int)
{
    return SetImpl(index, in_int);
}

DasResult IDasJsonImpl::SetFloatByIndex(size_t index, float in_float)
{
    return SetImpl(index, in_float);
}

DasResult IDasJsonImpl::SetStringByIndex(
    size_t              index,
    IDasReadOnlyString* p_in_string)
{
    DAS_UTILS_CHECK_POINTER(p_in_string)
    const auto expected_u8_value = ToU8StringWithoutOwnership(p_in_string);
    if (!expected_u8_value)
    {
        return expected_u8_value.error();
    }
    const auto p_u8_value = expected_u8_value.value();

    return SetImpl(index, std::string(p_u8_value));
}

DasResult IDasJsonImpl::SetBoolByIndex(size_t index, bool in_bool)
{
    return SetImpl(index, in_bool);
}

DasResult IDasJsonImpl::SetObjectByIndex(size_t index, IDasJson* p_in_das_json)
{
    DasPtr<IDasJsonImpl> p_impl;
    if (const auto qi_result = p_in_das_json->QueryInterface(
            DasIidOf<IDasJsonImpl>(),
            p_impl.PutVoid());
        DAS::IsFailed(qi_result))
    {
        return qi_result;
    }

    const auto in_json = std::visit(
        Utils::overload_set{
            [](IDasJsonImpl::Object& j) -> yyjson::value* { return &j.value_; },
            [](IDasJsonImpl::Ref& j) -> yyjson::value*
            {
                if (j.val_ == nullptr)
                {
                    return nullptr;
                }
                return j.val_;
            }},
        p_impl->impl_);

    if (in_json == nullptr)
    {
        return DAS_E_DANGLING_REFERENCE;
    }

    return SetImpl(index, *in_json);
}

// ========================================================================
// GetTypeByName / GetTypeByIndex
// ========================================================================

DasResult IDasJsonImpl::GetTypeByName(
    IDasReadOnlyString* key,
    DasType*            p_out_type)
{
    DAS_UTILS_CHECK_POINTER(key)

    const auto expected_u8_key = ToU8StringWithoutOwnership(key);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_u8_key = expected_u8_key.value();

    std::scoped_lock _{mutex_};
    try
    {
        auto val_opt = Details::GetValueByName(impl_, p_u8_key);
        if (!val_opt)
        {
            *p_out_type = DAS_TYPE_NULL;
            return DAS_S_OK;
        }
        const auto& val = val_opt.value();
        *p_out_type = Das::Utils::YyjsonValueToDasType(val);
        return DAS_S_OK;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
}

DasResult IDasJsonImpl::GetTypeByIndex(size_t index, DasType* p_out_type)
{
    DAS_UTILS_CHECK_POINTER(p_out_type)

    std::scoped_lock _{mutex_};
    try
    {
        auto val_opt = Details::GetValueByIndex(impl_, index);
        if (!val_opt)
        {
            return DAS_E_OUT_OF_RANGE;
        }
        const auto& val = val_opt.value();
        *p_out_type = Das::Utils::YyjsonValueToDasType(val);
        return DAS_S_OK;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const DasJsonImplJsonIsNotArray& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_TYPE_ERROR;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
}

// ========================================================================
// SetConnection / OnExpired
// ========================================================================

void IDasJsonImpl::SetConnection(const boost::signals2::connection& connection)
{
    if (auto* const p_ref = std::get_if<Ref>(&impl_); p_ref)
    {
        p_ref->connection_ = connection;
        return;
    }
    DAS_CORE_LOG_ERROR("Expect Ref but found Object!");
}

void IDasJsonImpl::OnExpired()
{
    std::visit(
        Utils::overload_set{
            [](const Object&)
            {
                DAS_CORE_LOG_ERROR(
                    "Type not matched. Expected reference but instance found.");
            },
            [](Ref& ref) { ref.val_ = nullptr; }},
        impl_);
}

// ========================================================================
// ToString
// ========================================================================

DasResult IDasJsonImpl::ToString(
    int32_t              indent,
    IDasReadOnlyString** pp_out_string)
{
    DAS_UTILS_CHECK_POINTER(pp_out_string)

    std::scoped_lock _{mutex_};
    try
    {
        return std::visit(
            Utils::overload_set{
                [pp_out_string, indent](Object& obj) -> DasResult
                {
                    auto serialized = Das::Utils::SerializeYyjsonValue(
                        obj.value_,
                        indent >= 0);
                    if (!serialized)
                    {
                        return DAS_E_INVALID_JSON;
                    }
                    return ::CreateIDasReadOnlyStringFromUtf8(
                        serialized.value().c_str(),
                        pp_out_string);
                },
                [pp_out_string, indent](Ref& ref) -> DasResult
                {
                    if (ref.val_ == nullptr)
                    {
                        return DAS_E_DANGLING_REFERENCE;
                    }
                    auto serialized = Das::Utils::SerializeYyjsonValue(
                        *ref.val_,
                        indent >= 0);
                    if (!serialized)
                    {
                        return DAS_E_INVALID_JSON;
                    }
                    return ::CreateIDasReadOnlyStringFromUtf8(
                        serialized.value().c_str(),
                        pp_out_string);
                }},
            impl_);
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ========================================================================
// GetSize
// ========================================================================

DasResult IDasJsonImpl::GetSize(uint64_t* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size)

    std::scoped_lock _{mutex_};
    try
    {
        *p_out_size = std::visit(
            Utils::overload_set{
                [](const Object& obj) -> uint64_t
                {
                    if (auto arr_opt = obj.value_.as_array())
                    {
                        return static_cast<uint64_t>(arr_opt->size());
                    }
                    if (auto obj_opt = obj.value_.as_object())
                    {
                        return static_cast<uint64_t>(obj_opt->size());
                    }
                    return 0;
                },
                [](const Ref& ref) -> uint64_t
                {
                    if (ref.val_ == nullptr)
                    {
                        throw DasJsonImplRefExpiredException{};
                    }
                    if (auto arr_opt = ref.val_->as_array())
                    {
                        return static_cast<uint64_t>(arr_opt->size());
                    }
                    if (auto obj_opt = ref.val_->as_object())
                    {
                        return static_cast<uint64_t>(obj_opt->size());
                    }
                    return 0;
                }},
            impl_);
        return DAS_S_OK;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ========================================================================
// Clear
// ========================================================================

DasResult IDasJsonImpl::Clear()
{
    std::scoped_lock _{mutex_};
    try
    {
        return std::visit(
            Utils::overload_set{
                [](Object& obj) -> DasResult
                {
                    obj.signal_();
                    obj.value_ = yyjson::object{};
                    return DAS_S_OK;
                },
                [](Ref& ref) -> DasResult
                {
                    if (ref.val_ != nullptr)
                    {
                        *ref.val_ = yyjson::object{};
                    }
                    return DAS_E_DANGLING_REFERENCE;
                }},
            impl_);
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ========================================================================
// C API — ParseDasJsonFromString
// ========================================================================

// ========================================================================
// C API — CreateDasJsonFromYyjson
// ========================================================================

DasResult CreateDasJsonFromYyjson(
    const yyjson::value&        value,
    ExportInterface::IDasJson** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    *pp_out = nullptr;
    try
    {
        const auto p_result =
            DAS::MakeDasPtr<IDasJsonImpl>(yyjson::value(value));
        DAS::Utils::SetResult(p_result, pp_out);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_CORE_UTILS_NS_END

DAS_C_API DasResult ParseDasJsonFromString(
    const char*                      p_u8_string,
    DAS::ExportInterface::IDasJson** pp_out_json)
{
    DAS_UTILS_CHECK_POINTER(p_u8_string)
    DAS_UTILS_CHECK_POINTER(pp_out_json)

    try
    {
        const auto p_result =
            DAS::MakeDasPtr<DAS::Core::Utils::IDasJsonImpl>(p_u8_string);
        DAS::Utils::SetResult(p_result, pp_out_json);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (const std::exception& ex)
    {
        DAS_CORE_LOG_ERROR("Parse json failed. What = {}", ex.what());
        DAS_CORE_LOG_ERROR("json = {}", p_u8_string);
        return DAS_E_INVALID_JSON;
    }
}

DAS_C_API DasResult
CreateEmptyDasJson(Das::ExportInterface::IDasJson** pp_out_json)
{
    DAS_UTILS_CHECK_POINTER(pp_out_json)

    try
    {
        const auto p_result = DAS::MakeDasPtr<DAS::Core::Utils::IDasJsonImpl>();
        DAS::Utils::SetResult(p_result, pp_out_json);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}
