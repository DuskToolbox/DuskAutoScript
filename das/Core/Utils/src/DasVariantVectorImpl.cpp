#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/Config.h>
// clang-format off
#include <das/PluginInterface/IDasComponent.h>
#include <das/ExportInterface/IDasVariantVector.h>
// clang-format on
#include <das/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/QueryInterface.hpp>
#include <magic_enum_format.hpp>
#include <variant>

DAS_CORE_UTILS_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto MakeSuccess(DasVariantType e) -> DasRetVariantType
{
    return {DAS_S_OK, e};
}

template <class V, class T>
DasResult Get(const V& v, T* p_out_t)
{
    const auto p_value = std::get_if<T>(&v);
    if (p_value == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error found. Expected type = {}",
            static_cast<DasVariantType>(v.index()));
        return DAS_E_TYPE_ERROR;
    }

    *p_out_t = *p_value;
    return DAS_S_OK;
}

template <class V>
DasResult Get(const V& v, IDasReadOnlyString** pp_out_t)
{
    const auto p_value = std::get_if<DasReadOnlyString>(&v);
    if (p_value == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error found. Expected type = {}",
            static_cast<DasVariantType>(v.index()));
        return DAS_E_TYPE_ERROR;
    }

    auto& out_t = *pp_out_t;
    out_t = p_value->Get();
    out_t->AddRef();
    return DAS_S_OK;
}

template <class V, class T>
DasResult Get(const V& v, T** pp_out_t)
{
    const auto p_value = std::get_if<T>(&v);
    if (p_value == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "Type error found. Expected type = {}",
            static_cast<DasVariantType>(v.index()));
        return DAS_E_TYPE_ERROR;
    }

    auto& out_t = *pp_out_t;
    out_t = p_value->Get();
    out_t->AddRef();
    return DAS_S_OK;
}

template <class T, class OtherT, class V>
DasResult Get2(const V& v, T** pp_out_t)
{
    DAS::DasPtr<T> value;
    auto           p_value = std::get_if<DAS::DasPtr<T>>(&v);
    if (p_value == nullptr)
    {
        auto p_other_value = std::get_if<DAS::DasPtr<OtherT>>(&v);
        if (p_other_value == nullptr)
        {
            DAS_CORE_LOG_ERROR(
                "Type error! Expected type = {}",
                static_cast<DasVariantType>(v.index()));
            return DAS_E_TYPE_ERROR;
        }
        auto expected_value =
            DAS::Core::ForeignInterfaceHost::MakeInterop<T>(*p_other_value);
        if (!expected_value)
        {
            return expected_value.error();
        }
        value = expected_value.value();
        p_value = &value;
    }

    auto& out_t = *pp_out_t;
    out_t = p_value->Get();
    out_t->AddRef();
    return DAS_S_OK;
}

DAS_NS_ANONYMOUS_DETAILS_END

class DasVariantVectorImpl;

class IDasVariantVectorImpl final : public IDasVariantVector
{
public:
    IDasVariantVectorImpl(DasVariantVectorImpl& impl);

    int64_t   AddRef() override;
    int64_t   Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;

    DasResult GetInt(size_t index, int64_t* p_out_int) override;
    DasResult GetFloat(size_t index, float* p_out_float) override;
    DasResult GetString(size_t index, IDasReadOnlyString** pp_out_string)
        override;
    DasResult GetBool(size_t index, bool* p_out_bool) override;
    DasResult GetComponent(size_t index, IDasComponent** pp_out_component)
        override;
    DasResult GetBase(size_t index, IDasBase** pp_out_base) override;

    DasResult SetInt(size_t index, int64_t in_int) override;
    DasResult SetFloat(size_t index, float in_float) override;
    DasResult SetString(size_t index, IDasReadOnlyString* in_string) override;
    DasResult SetBool(size_t index, bool in_bool) override;
    DasResult SetComponent(size_t index, IDasComponent* in_component) override;
    DasResult SetBase(size_t index, IDasBase* in_base) override;

    DasResult PushBackInt(int64_t in_int) override;
    DasResult PushBackFloat(float in_float) override;
    DasResult PushBackString(IDasReadOnlyString* in_string) override;
    DasResult PushBackBool(bool in_bool) override;
    DasResult PushBackComponent(IDasComponent* in_component) override;
    DasResult PushBackBase(IDasBase* in_base) override;

    DasResult GetType(size_t index, DasVariantType* p_out_type) override;
    DasResult RemoveAt(size_t index) override;
    DasResult GetSize() override;

private:
    DasVariantVectorImpl& impl_;
};

class IDasSwigVariantVectorImpl final : public IDasSwigVariantVector
{
public:
    IDasSwigVariantVectorImpl(DasVariantVectorImpl& impl);

    int64_t        AddRef() override;
    int64_t        Release() override;
    DasRetSwigBase QueryInterface(const DasGuid& iid) override;

    DasRetInt            GetInt(size_t index) override;
    DasRetFloat          GetFloat(size_t index) override;
    DasRetReadOnlyString GetString(size_t index) override;
    DasRetBool           GetBool(size_t index) override;
    DasRetComponent      GetComponent(size_t index) override;
    DasRetSwigBase       GetBase(size_t index) override;

    DasResult SetInt(size_t index, int64_t in_int) override;
    DasResult SetFloat(size_t index, float in_float) override;
    DasResult SetString(size_t index, DasReadOnlyString in_string) override;
    DasResult SetBool(size_t index, bool in_bool) override;
    DasResult SetComponent(size_t index, IDasSwigComponent* in_component)
        override;
    DasResult SetBase(size_t index, IDasSwigBase* in_base) override;

    DasResult PushBackInt(int64_t in_int) override;
    DasResult PushBackFloat(float in_float) override;
    DasResult PushBackString(DasReadOnlyString in_string) override;
    DasResult PushBackBool(bool in_bool) override;
    DasResult PushBackComponent(IDasSwigComponent* in_component) override;
    DasResult PushBackBase(IDasSwigBase* in_base) override;

    DasRetVariantType GetType(size_t index) override;
    DasResult         RemoveAt(size_t index) override;
    DasResult         GetSize() override;

private:
    DasVariantVectorImpl& impl_;
};

class DasVariantVectorImpl final : DAS_UTILS_MULTIPLE_PROJECTION_GENERATORS(
                                       DasVariantVectorImpl,
                                       IDasVariantVectorImpl,
                                       IDasSwigVariantVectorImpl)
{
    using Variant = std::variant<
        int64_t,
        float,
        bool,
        DasReadOnlyString,
        DasPtr<IDasBase>,
        DasPtr<IDasSwigBase>,
        DasPtr<IDasComponent>,
        DasPtr<IDasSwigComponent>>;
    RefCounter<DasVariantVectorImpl> ref_counter_{};
    std::vector<Variant>             variants_{};

public:
    int64_t   AddRef();
    int64_t   Release();
    DasResult QueryInterface(const DasGuid& iid, void** pp_object);

    DasResult GetInt(size_t index, int64_t* p_out_int);
    DasResult GetFloat(size_t index, float* p_out_float);
    DasResult GetString(size_t index, IDasReadOnlyString** pp_out_string);
    DasResult GetBool(size_t index, bool* p_out_bool);
    DasResult GetComponent(size_t index, IDasComponent** pp_out_component);
    DasResult GetBase(size_t index, IDasBase** pp_out_base);
    DasRetComponent GetComponent(size_t index);
    DasRetSwigBase  GetBase(size_t index);

    DasResult SetInt(size_t index, int64_t in_int);
    DasResult SetFloat(size_t index, float in_float);
    DasResult SetString(size_t index, IDasReadOnlyString* in_string);
    DasResult SetBool(size_t index, bool in_bool);
    DasResult SetComponent(size_t index, IDasComponent* in_component);
    DasResult SetBase(size_t index, IDasBase* in_base);
    DasResult SetComponent(size_t index, IDasSwigComponent* in_component);
    DasResult SetBase(size_t index, IDasSwigBase* in_base);

    DasResult PushBackInt(int64_t in_int);
    DasResult PushBackFloat(float in_float);
    DasResult PushBackString(IDasReadOnlyString* in_string);
    DasResult PushBackBool(bool in_bool);
    DasResult PushBackComponent(IDasComponent* in_component);
    DasResult PushBackBase(IDasBase* in_base);
    DasResult PushBackComponent(IDasSwigComponent* in_component);
    DasResult PushBackBase(IDasSwigBase* in_base);

    DasResult GetType(size_t index, DasVariantType* p_out_type);
    DasResult RemoveAt(size_t index);
    DasResult GetSize();

    static DasRetVariantType ToType(const Variant& v);
};

IDasVariantVectorImpl::IDasVariantVectorImpl(DasVariantVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IDasVariantVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasVariantVectorImpl::Release() { return impl_.Release(); }

DasResult IDasVariantVectorImpl::GetInt(size_t index, int64_t* p_out_int)
{
    return impl_.GetInt(index, p_out_int);
}

DasResult IDasVariantVectorImpl::GetFloat(size_t index, float* p_out_float)
{
    return impl_.GetFloat(index, p_out_float);
}

DasResult IDasVariantVectorImpl::GetString(
    size_t               index,
    IDasReadOnlyString** pp_out_string)
{
    return impl_.GetString(index, pp_out_string);
}

DasResult IDasVariantVectorImpl::GetBool(size_t index, bool* p_out_bool)
{
    return impl_.GetBool(index, p_out_bool);
}

DasResult IDasVariantVectorImpl::GetComponent(
    size_t          index,
    IDasComponent** pp_out_component)
{
    return impl_.GetComponent(index, pp_out_component);
}

DasResult IDasVariantVectorImpl::GetBase(size_t index, IDasBase** pp_out_base)
{
    return impl_.GetBase(index, pp_out_base);
}

DasResult IDasVariantVectorImpl::SetInt(size_t index, int64_t in_int)
{
    return impl_.SetInt(index, in_int);
}

DasResult IDasVariantVectorImpl::SetFloat(size_t index, float in_float)
{
    return impl_.SetFloat(index, in_float);
}

DasResult IDasVariantVectorImpl::SetString(
    size_t              index,
    IDasReadOnlyString* in_string)
{
    return impl_.SetString(index, in_string);
}

DasResult IDasVariantVectorImpl::SetBool(size_t index, bool in_bool)
{
    return impl_.SetBool(index, in_bool);
}

DasResult IDasVariantVectorImpl::SetComponent(
    size_t         index,
    IDasComponent* in_component)
{
    return impl_.SetComponent(index, in_component);
}

DasResult IDasVariantVectorImpl::SetBase(size_t index, IDasBase* in_base)
{
    return impl_.SetBase(index, in_base);
}

DasResult IDasVariantVectorImpl::PushBackInt(int64_t in_int)
{
    return impl_.PushBackInt(in_int);
}

DasResult IDasVariantVectorImpl::PushBackFloat(float in_float)
{
    return impl_.PushBackFloat(in_float);
}

DasResult IDasVariantVectorImpl::PushBackString(IDasReadOnlyString* in_string)
{
    return impl_.PushBackString(in_string);
}

DasResult IDasVariantVectorImpl::PushBackBool(bool in_bool)
{
    return impl_.PushBackBool(in_bool);
}

DasResult IDasVariantVectorImpl::PushBackComponent(IDasComponent* in_component)
{
    return impl_.PushBackComponent(in_component);
}

DasResult IDasVariantVectorImpl::PushBackBase(IDasBase* in_base)
{
    return impl_.PushBackBase(in_base);
}

DasResult IDasVariantVectorImpl::GetType(
    size_t          index,
    DasVariantType* p_out_type)
{
    return impl_.GetType(index, p_out_type);
}

DasResult IDasVariantVectorImpl::RemoveAt(size_t index)
{
    return impl_.RemoveAt(index);
}

DasResult IDasVariantVectorImpl::GetSize() { return impl_.GetSize(); }

IDasSwigVariantVectorImpl::IDasSwigVariantVectorImpl(DasVariantVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IDasSwigVariantVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasSwigVariantVectorImpl::Release() { return impl_.Release(); }

DasRetSwigBase IDasSwigVariantVectorImpl::QueryInterface(const DasGuid& iid)
{
    DasRetSwigBase result{};
    result.error_code = impl_.QueryInterface(iid, &result.value);
    return result;
}

DasRetInt IDasSwigVariantVectorImpl::GetInt(size_t index)
{
    DasRetInt result;
    result.error_code = impl_.GetInt(index, &result.value);
    return result;
}

DasRetFloat IDasSwigVariantVectorImpl::GetFloat(size_t index)
{
    DasRetFloat result;
    result.error_code = impl_.GetFloat(index, &result.value);
    return result;
}

DasRetReadOnlyString IDasSwigVariantVectorImpl::GetString(size_t index)
{
    DasRetReadOnlyString       result;
    DasPtr<IDasReadOnlyString> p_value;
    result.error_code = impl_.GetString(index, p_value.Put());
    result.value = p_value;
    return result;
}

DasRetBool IDasSwigVariantVectorImpl::GetBool(size_t index)
{
    DasRetBool result;
    result.error_code = impl_.GetBool(index, &result.value);
    return result;
}

DasRetComponent IDasSwigVariantVectorImpl::GetComponent(size_t index)
{
    return impl_.GetComponent(index);
}

DasRetSwigBase IDasSwigVariantVectorImpl::GetBase(size_t index)
{
    return impl_.GetBase(index);
}

DasResult IDasSwigVariantVectorImpl::SetInt(size_t index, int64_t in_int)
{
    return impl_.SetInt(index, in_int);
}

DasResult IDasSwigVariantVectorImpl::SetFloat(size_t index, float in_float)
{
    return impl_.SetFloat(index, in_float);
}

DasResult IDasSwigVariantVectorImpl::SetString(
    size_t            index,
    DasReadOnlyString in_string)
{
    return impl_.SetString(index, in_string.Get());
}

DasResult IDasSwigVariantVectorImpl::SetBool(size_t index, bool in_bool)
{
    return impl_.SetBool(index, in_bool);
}

DasResult IDasSwigVariantVectorImpl::SetComponent(
    size_t             index,
    IDasSwigComponent* in_component)
{
    return impl_.SetComponent(index, in_component);
}
DasResult IDasSwigVariantVectorImpl::SetBase(
    size_t        index,
    IDasSwigBase* in_base)
{
    return impl_.SetBase(index, in_base);
}

DasResult IDasSwigVariantVectorImpl::PushBackInt(int64_t in_int)
{
    return impl_.PushBackInt(in_int);
}

DasResult IDasSwigVariantVectorImpl::PushBackFloat(float in_float)
{
    return impl_.PushBackFloat(in_float);
}

DasResult IDasSwigVariantVectorImpl::PushBackString(DasReadOnlyString in_string)
{
    return impl_.PushBackString(in_string.Get());
}

DasResult IDasSwigVariantVectorImpl::PushBackBool(bool in_bool)
{
    return impl_.PushBackBool(in_bool);
}

DasResult IDasSwigVariantVectorImpl::PushBackComponent(
    IDasSwigComponent* in_component)
{
    return impl_.PushBackComponent(in_component);
}

DasResult IDasSwigVariantVectorImpl::PushBackBase(IDasSwigBase* in_base)
{
    return impl_.PushBackBase(in_base);
}

DasRetVariantType IDasSwigVariantVectorImpl::GetType(size_t index)
{
    DasRetVariantType result;
    result.error_code = impl_.GetType(index, &result.value);
    return result;
}

DasResult IDasSwigVariantVectorImpl::RemoveAt(size_t index)
{
    return impl_.RemoveAt(index);
}

DasResult IDasSwigVariantVectorImpl::GetSize() { return impl_.GetSize(); }

DasResult IDasVariantVectorImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    return impl_.QueryInterface(iid, pp_object);
}

int64_t DasVariantVectorImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t DasVariantVectorImpl::Release() { return ref_counter_.Release(this); }

DasResult DasVariantVectorImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    const auto cpp_qi_result = Utils::QueryInterface<IDasVariantVector>(
        static_cast<IDasVariantVectorImpl*>(*this),
        iid,
        pp_object);
    if (IsFailed(cpp_qi_result))
    {
        if (cpp_qi_result == DAS_E_NO_INTERFACE)
        {
            const auto swig_qi_result =
                Utils::QueryInterface<IDasSwigVariantVector>(
                    static_cast<IDasSwigVariantVectorImpl*>(*this),
                    iid,
                    pp_object);
            return swig_qi_result;
        }
        return cpp_qi_result;
    }
    return cpp_qi_result;
}

DasResult DasVariantVectorImpl::GetInt(size_t index, int64_t* p_out_int)
{
    DAS_UTILS_CHECK_POINTER(p_out_int)

    if (index < variants_.size())
    {
        return Details::Get(variants_[index], p_out_int);
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::GetFloat(size_t index, float* p_out_float)
{
    DAS_UTILS_CHECK_POINTER(p_out_float)

    if (index < variants_.size())
    {
        return Details::Get(variants_[index], p_out_float);
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::GetString(
    size_t               index,
    IDasReadOnlyString** pp_out_string)
{
    DAS_UTILS_CHECK_POINTER(pp_out_string)

    if (index < variants_.size())
    {
        return Details::Get(variants_[index], pp_out_string);
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::GetBool(size_t index, bool* p_out_bool)
{
    DAS_UTILS_CHECK_POINTER(p_out_bool)

    if (index < variants_.size())
    {
        return Details::Get(variants_[index], p_out_bool);
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::GetComponent(
    size_t          index,
    IDasComponent** pp_out_component)
{
    DAS_UTILS_CHECK_POINTER(pp_out_component)

    if (index < variants_.size())
    {
        return Details::Get2<IDasComponent, IDasSwigComponent>(
            variants_[index],
            pp_out_component);
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::GetBase(size_t index, IDasBase** pp_out_base)
{
    DAS_UTILS_CHECK_POINTER(pp_out_base)

    if (index < variants_.size())
    {
        return Details::Get2<IDasBase, IDasSwigBase>(
            variants_[index],
            pp_out_base);
    }
    return DAS_E_OUT_OF_RANGE;
}

DasRetComponent DasVariantVectorImpl::GetComponent(size_t index)
{
    if (index < variants_.size())
    {
        DasRetComponent result;
        result.error_code = Details::Get2<IDasSwigComponent, IDasComponent>(
            variants_[index],
            result.value.Put());
        return result;
    }
    return {DAS_E_OUT_OF_RANGE};
}

DasRetSwigBase DasVariantVectorImpl::GetBase(size_t index)
{
    if (index < variants_.size())
    {
        DasRetSwigBase result;
        IDasSwigBase* p_base;
        result.error_code =
            Details::Get2<IDasSwigBase, IDasBase>(variants_[index], &p_base);
        result.value = p_base;
        return result;
    }
    return {DAS_E_OUT_OF_RANGE};
}

DasResult DasVariantVectorImpl::SetInt(size_t index, int64_t in_int)
{

    if (index < variants_.size())
    {
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
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::SetFloat(size_t index, float in_float)
{

    if (index < variants_.size())
    {
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
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::SetString(
    size_t              index,
    IDasReadOnlyString* in_string)
{
    DAS_UTILS_CHECK_POINTER(in_string)

    if (index < variants_.size())
    {
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
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::SetBool(size_t index, bool in_bool)
{

    if (index < variants_.size())
    {
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
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::SetComponent(
    size_t         index,
    IDasComponent* in_component)
{
    DAS_UTILS_CHECK_POINTER(in_component)

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = DasPtr{in_component};
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            DAS_CORE_LOG_ERROR("Out of memory!");
            return DAS_E_OUT_OF_MEMORY;
        }
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::SetBase(size_t index, IDasBase* in_base)
{
    DAS_UTILS_CHECK_POINTER(in_base)

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = DasPtr{in_base};
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            DAS_CORE_LOG_ERROR("Out of memory!");
            return DAS_E_OUT_OF_MEMORY;
        }
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::SetComponent(
    size_t             index,
    IDasSwigComponent* in_component)
{
    DAS_UTILS_CHECK_POINTER(in_component)

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = DasPtr{in_component};
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            DAS_CORE_LOG_ERROR("Out of memory!");
            return DAS_E_OUT_OF_MEMORY;
        }
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::SetBase(size_t index, IDasSwigBase* in_base)
{
    DAS_UTILS_CHECK_POINTER(in_base)

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = DasPtr{in_base};
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            DAS_CORE_LOG_ERROR("Out of memory!");
            return DAS_E_OUT_OF_MEMORY;
        }
    }
    return DAS_E_OUT_OF_RANGE;
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

DasResult DasVariantVectorImpl::PushBackString(IDasReadOnlyString* in_string)
{
    DAS_UTILS_CHECK_POINTER(in_string)

    try
    {
        variants_.emplace_back(DasReadOnlyString(in_string));
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
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

DasResult DasVariantVectorImpl::PushBackComponent(IDasComponent* in_component)
{
    DAS_UTILS_CHECK_POINTER(in_component)

    try
    {
        variants_.emplace_back(DasPtr{in_component});
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasVariantVectorImpl::PushBackBase(IDasBase* in_base)
{
    DAS_UTILS_CHECK_POINTER(in_base)

    try
    {
        variants_.emplace_back(DasPtr{in_base});
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasVariantVectorImpl::PushBackComponent(IDasSwigComponent* in_component)
{
    DAS_UTILS_CHECK_POINTER(in_component)

    try
    {
        variants_.emplace_back(DasPtr{in_component});
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasVariantVectorImpl::PushBackBase(IDasSwigBase* in_base)
{
    DAS_UTILS_CHECK_POINTER(in_base)

    try
    {
        variants_.emplace_back(DasPtr{in_base});
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasVariantVectorImpl::GetType(
    size_t          index,
    DasVariantType* p_out_type)
{
    DAS_UTILS_CHECK_POINTER(p_out_type)

    if (index < variants_.size())
    {
        const auto ret_type = ToType(variants_[index]);
        if (IsOk(ret_type.error_code))
        {

            *p_out_type = ret_type.value;
            return ret_type.error_code;
        }
        return ret_type.error_code;
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::RemoveAt(size_t index)
{
    if (index < variants_.size())
    {
        variants_.erase(variants_.begin() + index);
        return DAS_S_OK;
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasVariantVectorImpl::GetSize()
{
    const auto size = variants_.size();
    if (size > std::numeric_limits<DasResult>::max())
    {
        DAS_CORE_LOG_ERROR("Overflow detected! Size = {}.", size);
    }
    return static_cast<DasResult>(size);
}

DasRetVariantType DasVariantVectorImpl::ToType(const Variant& v)
{
    const auto i = v.index();
    switch (i)
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
        [[fallthrough]];
    case 5:
        return Details::MakeSuccess(DAS_VARIANT_TYPE_BASE);
    case 6:
        [[fallthrough]];
    case 7:
        return Details::MakeSuccess(DAS_VARIANT_TYPE_COMPONENT);
    default:
        DAS_CORE_LOG_ERROR("Unexpected enum found. Value = {}", i);
        return {DAS_E_INTERNAL_FATAL_ERROR, DAS_VARIANT_TYPE_FORCE_DWORD};
    }
}

DAS_CORE_UTILS_NS_END
