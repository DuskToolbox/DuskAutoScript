#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/Core/Utils/Config.h>
// clang-format off
#include <AutoStarRail/PluginInterface/IAsrComponent.h>
#include <AutoStarRail/ExportInterface/IAsrVariantVector.h>
// clang-format on
#include <AutoStarRail/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <AutoStarRail/Utils/CommonUtils.hpp>
#include <AutoStarRail/Utils/QueryInterface.hpp>
#include <magic_enum_format.hpp>
#include <variant>

ASR_CORE_UTILS_NS_BEGIN

ASR_NS_ANONYMOUS_DETAILS_BEGIN

auto MakeSuccess(AsrVariantType e) -> AsrRetVariantType
{
    return {ASR_S_OK, e};
}

template <class V, class T>
AsrResult Get(const V& v, T* p_out_t)
{
    const auto p_value = std::get_if<T>(&v);
    if (p_value == nullptr)
    {
        ASR_CORE_LOG_ERROR(
            "Type error found. Expected type = {}",
            static_cast<AsrVariantType>(v.index()));
        return ASR_E_TYPE_ERROR;
    }

    *p_out_t = *p_value;
    return ASR_S_OK;
}

template <class V>
AsrResult Get(const V& v, IAsrReadOnlyString** pp_out_t)
{
    const auto p_value = std::get_if<AsrReadOnlyString>(&v);
    if (p_value == nullptr)
    {
        ASR_CORE_LOG_ERROR(
            "Type error found. Expected type = {}",
            static_cast<AsrVariantType>(v.index()));
        return ASR_E_TYPE_ERROR;
    }

    auto& out_t = *pp_out_t;
    out_t = p_value->Get();
    out_t->AddRef();
    return ASR_S_OK;
}

template <class V, class T>
AsrResult Get(const V& v, T** pp_out_t)
{
    const auto p_value = std::get_if<T>(&v);
    if (p_value == nullptr)
    {
        ASR_CORE_LOG_ERROR(
            "Type error found. Expected type = {}",
            static_cast<AsrVariantType>(v.index()));
        return ASR_E_TYPE_ERROR;
    }

    auto& out_t = *pp_out_t;
    out_t = p_value->Get();
    out_t->AddRef();
    return ASR_S_OK;
}

template <class T, class OtherT, class V>
AsrResult Get2(const V& v, T** pp_out_t)
{
    ASR::AsrPtr<T> value;
    auto           p_value = std::get_if<ASR::AsrPtr<T>>(&v);
    if (p_value == nullptr)
    {
        auto p_other_value = std::get_if<ASR::AsrPtr<OtherT>>(&v);
        if (p_other_value == nullptr)
        {
            ASR_CORE_LOG_ERROR(
                "Type error! Expected type = {}",
                static_cast<AsrVariantType>(v.index()));
            return ASR_E_TYPE_ERROR;
        }
        auto expected_value =
            ASR::Core::ForeignInterfaceHost::MakeInterop<T>(*p_other_value);
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
    return ASR_S_OK;
}

ASR_NS_ANONYMOUS_DETAILS_END

class AsrVariantVectorImpl;

class IAsrVariantVectorImpl final : public IAsrVariantVector
{
public:
    IAsrVariantVectorImpl(AsrVariantVectorImpl& impl);

    int64_t   AddRef() override;
    int64_t   Release() override;
    AsrResult QueryInterface(const AsrGuid& iid, void** pp_object) override;

    AsrResult GetInt(size_t index, int64_t* p_out_int) override;
    AsrResult GetFloat(size_t index, float* p_out_float) override;
    AsrResult GetString(size_t index, IAsrReadOnlyString** pp_out_string)
        override;
    AsrResult GetBool(size_t index, bool* p_out_bool) override;
    AsrResult GetComponent(size_t index, IAsrComponent** pp_out_component)
        override;
    AsrResult GetBase(size_t index, IAsrBase** pp_out_base) override;

    AsrResult SetInt(size_t index, int64_t in_int) override;
    AsrResult SetFloat(size_t index, float in_float) override;
    AsrResult SetString(size_t index, IAsrReadOnlyString* in_string) override;
    AsrResult SetBool(size_t index, bool in_bool) override;
    AsrResult SetComponent(size_t index, IAsrComponent* in_component) override;
    AsrResult SetBase(size_t index, IAsrBase* in_base) override;

    AsrResult PushBackInt(int64_t in_int) override;
    AsrResult PushBackFloat(float in_float) override;
    AsrResult PushBackString(IAsrReadOnlyString* in_string) override;
    AsrResult PushBackBool(bool in_bool) override;
    AsrResult PushBackComponent(IAsrComponent* in_component) override;
    AsrResult PushBackBase(IAsrBase* in_base) override;

    AsrResult GetType(size_t index, AsrVariantType* p_out_type) override;
    AsrResult RemoveAt(size_t index) override;
    AsrResult GetSize() override;

private:
    AsrVariantVectorImpl& impl_;
};

class IAsrSwigVariantVectorImpl final : public IAsrSwigVariantVector
{
public:
    IAsrSwigVariantVectorImpl(AsrVariantVectorImpl& impl);

    int64_t        AddRef() override;
    int64_t        Release() override;
    AsrRetSwigBase QueryInterface(const AsrGuid& iid) override;

    AsrRetInt            GetInt(size_t index) override;
    AsrRetFloat          GetFloat(size_t index) override;
    AsrRetReadOnlyString GetString(size_t index) override;
    AsrRetBool           GetBool(size_t index) override;
    AsrRetComponent      GetComponent(size_t index) override;
    AsrRetSwigBase       GetBase(size_t index) override;

    AsrResult SetInt(size_t index, int64_t in_int) override;
    AsrResult SetFloat(size_t index, float in_float) override;
    AsrResult SetString(size_t index, AsrReadOnlyString in_string) override;
    AsrResult SetBool(size_t index, bool in_bool) override;
    AsrResult SetComponent(size_t index, IAsrSwigComponent* in_component)
        override;
    AsrResult SetBase(size_t index, IAsrSwigBase* in_base) override;

    AsrResult PushBackInt(int64_t in_int) override;
    AsrResult PushBackFloat(float in_float) override;
    AsrResult PushBackString(AsrReadOnlyString in_string) override;
    AsrResult PushBackBool(bool in_bool) override;
    AsrResult PushBackComponent(IAsrSwigComponent* in_component) override;
    AsrResult PushBackBase(IAsrSwigBase* in_base) override;

    AsrRetVariantType GetType(size_t index) override;
    AsrResult         RemoveAt(size_t index) override;
    AsrResult         GetSize() override;

private:
    AsrVariantVectorImpl& impl_;
};

class AsrVariantVectorImpl final : ASR_UTILS_MULTIPLE_PROJECTION_GENERATORS(
                                       AsrVariantVectorImpl,
                                       IAsrVariantVectorImpl,
                                       IAsrSwigVariantVectorImpl)
{
    using Variant = std::variant<
        int64_t,
        float,
        bool,
        AsrReadOnlyString,
        AsrPtr<IAsrBase>,
        AsrPtr<IAsrSwigBase>,
        AsrPtr<IAsrComponent>,
        AsrPtr<IAsrSwigComponent>>;
    RefCounter<AsrVariantVectorImpl> ref_counter_{};
    std::vector<Variant>             variants_{};

public:
    int64_t   AddRef();
    int64_t   Release();
    AsrResult QueryInterface(const AsrGuid& iid, void** pp_object);

    AsrResult GetInt(size_t index, int64_t* p_out_int);
    AsrResult GetFloat(size_t index, float* p_out_float);
    AsrResult GetString(size_t index, IAsrReadOnlyString** pp_out_string);
    AsrResult GetBool(size_t index, bool* p_out_bool);
    AsrResult GetComponent(size_t index, IAsrComponent** pp_out_component);
    AsrResult GetBase(size_t index, IAsrBase** pp_out_base);
    AsrRetComponent GetComponent(size_t index);
    AsrRetSwigBase  GetBase(size_t index);

    AsrResult SetInt(size_t index, int64_t in_int);
    AsrResult SetFloat(size_t index, float in_float);
    AsrResult SetString(size_t index, IAsrReadOnlyString* in_string);
    AsrResult SetBool(size_t index, bool in_bool);
    AsrResult SetComponent(size_t index, IAsrComponent* in_component);
    AsrResult SetBase(size_t index, IAsrBase* in_base);
    AsrResult SetComponent(size_t index, IAsrSwigComponent* in_component);
    AsrResult SetBase(size_t index, IAsrSwigBase* in_base);

    AsrResult PushBackInt(int64_t in_int);
    AsrResult PushBackFloat(float in_float);
    AsrResult PushBackString(IAsrReadOnlyString* in_string);
    AsrResult PushBackBool(bool in_bool);
    AsrResult PushBackComponent(IAsrComponent* in_component);
    AsrResult PushBackBase(IAsrBase* in_base);
    AsrResult PushBackComponent(IAsrSwigComponent* in_component);
    AsrResult PushBackBase(IAsrSwigBase* in_base);

    AsrResult GetType(size_t index, AsrVariantType* p_out_type);
    AsrResult RemoveAt(size_t index);
    AsrResult GetSize();

    static AsrRetVariantType ToType(const Variant& v);
};

IAsrVariantVectorImpl::IAsrVariantVectorImpl(AsrVariantVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IAsrVariantVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IAsrVariantVectorImpl::Release() { return impl_.Release(); }

AsrResult IAsrVariantVectorImpl::GetInt(size_t index, int64_t* p_out_int)
{
    return impl_.GetInt(index, p_out_int);
}

AsrResult IAsrVariantVectorImpl::GetFloat(size_t index, float* p_out_float)
{
    return impl_.GetFloat(index, p_out_float);
}

AsrResult IAsrVariantVectorImpl::GetString(
    size_t               index,
    IAsrReadOnlyString** pp_out_string)
{
    return impl_.GetString(index, pp_out_string);
}

AsrResult IAsrVariantVectorImpl::GetBool(size_t index, bool* p_out_bool)
{
    return impl_.GetBool(index, p_out_bool);
}

AsrResult IAsrVariantVectorImpl::GetComponent(
    size_t          index,
    IAsrComponent** pp_out_component)
{
    return impl_.GetComponent(index, pp_out_component);
}

AsrResult IAsrVariantVectorImpl::GetBase(size_t index, IAsrBase** pp_out_base)
{
    return impl_.GetBase(index, pp_out_base);
}

AsrResult IAsrVariantVectorImpl::SetInt(size_t index, int64_t in_int)
{
    return impl_.SetInt(index, in_int);
}

AsrResult IAsrVariantVectorImpl::SetFloat(size_t index, float in_float)
{
    return impl_.SetFloat(index, in_float);
}

AsrResult IAsrVariantVectorImpl::SetString(
    size_t              index,
    IAsrReadOnlyString* in_string)
{
    return impl_.SetString(index, in_string);
}

AsrResult IAsrVariantVectorImpl::SetBool(size_t index, bool in_bool)
{
    return impl_.SetBool(index, in_bool);
}

AsrResult IAsrVariantVectorImpl::SetComponent(
    size_t         index,
    IAsrComponent* in_component)
{
    return impl_.SetComponent(index, in_component);
}

AsrResult IAsrVariantVectorImpl::SetBase(size_t index, IAsrBase* in_base)
{
    return impl_.SetBase(index, in_base);
}

AsrResult IAsrVariantVectorImpl::PushBackInt(int64_t in_int)
{
    return impl_.PushBackInt(in_int);
}

AsrResult IAsrVariantVectorImpl::PushBackFloat(float in_float)
{
    return impl_.PushBackFloat(in_float);
}

AsrResult IAsrVariantVectorImpl::PushBackString(IAsrReadOnlyString* in_string)
{
    return impl_.PushBackString(in_string);
}

AsrResult IAsrVariantVectorImpl::PushBackBool(bool in_bool)
{
    return impl_.PushBackBool(in_bool);
}

AsrResult IAsrVariantVectorImpl::PushBackComponent(IAsrComponent* in_component)
{
    return impl_.PushBackComponent(in_component);
}

AsrResult IAsrVariantVectorImpl::PushBackBase(IAsrBase* in_base)
{
    return impl_.PushBackBase(in_base);
}

AsrResult IAsrVariantVectorImpl::GetType(
    size_t          index,
    AsrVariantType* p_out_type)
{
    return impl_.GetType(index, p_out_type);
}

AsrResult IAsrVariantVectorImpl::RemoveAt(size_t index)
{
    return impl_.RemoveAt(index);
}

AsrResult IAsrVariantVectorImpl::GetSize() { return impl_.GetSize(); }

IAsrSwigVariantVectorImpl::IAsrSwigVariantVectorImpl(AsrVariantVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IAsrSwigVariantVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IAsrSwigVariantVectorImpl::Release() { return impl_.Release(); }

AsrRetSwigBase IAsrSwigVariantVectorImpl::QueryInterface(const AsrGuid& iid)
{
    AsrRetSwigBase result{};
    result.error_code = impl_.QueryInterface(iid, &result.value);
    return result;
}

AsrRetInt IAsrSwigVariantVectorImpl::GetInt(size_t index)
{
    AsrRetInt result;
    result.error_code = impl_.GetInt(index, &result.value);
    return result;
}

AsrRetFloat IAsrSwigVariantVectorImpl::GetFloat(size_t index)
{
    AsrRetFloat result;
    result.error_code = impl_.GetFloat(index, &result.value);
    return result;
}

AsrRetReadOnlyString IAsrSwigVariantVectorImpl::GetString(size_t index)
{
    AsrRetReadOnlyString       result;
    AsrPtr<IAsrReadOnlyString> p_value;
    result.error_code = impl_.GetString(index, p_value.Put());
    result.value = p_value;
    return result;
}

AsrRetBool IAsrSwigVariantVectorImpl::GetBool(size_t index)
{
    AsrRetBool result;
    result.error_code = impl_.GetBool(index, &result.value);
    return result;
}

AsrRetComponent IAsrSwigVariantVectorImpl::GetComponent(size_t index)
{
    return impl_.GetComponent(index);
}

AsrRetSwigBase IAsrSwigVariantVectorImpl::GetBase(size_t index)
{
    return impl_.GetBase(index);
}

AsrResult IAsrSwigVariantVectorImpl::SetInt(size_t index, int64_t in_int)
{
    return impl_.SetInt(index, in_int);
}

AsrResult IAsrSwigVariantVectorImpl::SetFloat(size_t index, float in_float)
{
    return impl_.SetFloat(index, in_float);
}

AsrResult IAsrSwigVariantVectorImpl::SetString(
    size_t            index,
    AsrReadOnlyString in_string)
{
    return impl_.SetString(index, in_string.Get());
}

AsrResult IAsrSwigVariantVectorImpl::SetBool(size_t index, bool in_bool)
{
    return impl_.SetBool(index, in_bool);
}

AsrResult IAsrSwigVariantVectorImpl::SetComponent(
    size_t             index,
    IAsrSwigComponent* in_component)
{
    return impl_.SetComponent(index, in_component);
}
AsrResult IAsrSwigVariantVectorImpl::SetBase(
    size_t        index,
    IAsrSwigBase* in_base)
{
    return impl_.SetBase(index, in_base);
}

AsrResult IAsrSwigVariantVectorImpl::PushBackInt(int64_t in_int)
{
    return impl_.PushBackInt(in_int);
}

AsrResult IAsrSwigVariantVectorImpl::PushBackFloat(float in_float)
{
    return impl_.PushBackFloat(in_float);
}

AsrResult IAsrSwigVariantVectorImpl::PushBackString(AsrReadOnlyString in_string)
{
    return impl_.PushBackString(in_string.Get());
}

AsrResult IAsrSwigVariantVectorImpl::PushBackBool(bool in_bool)
{
    return impl_.PushBackBool(in_bool);
}

AsrResult IAsrSwigVariantVectorImpl::PushBackComponent(
    IAsrSwigComponent* in_component)
{
    return impl_.PushBackComponent(in_component);
}

AsrResult IAsrSwigVariantVectorImpl::PushBackBase(IAsrSwigBase* in_base)
{
    return impl_.PushBackBase(in_base);
}

AsrRetVariantType IAsrSwigVariantVectorImpl::GetType(size_t index)
{
    AsrRetVariantType result;
    result.error_code = impl_.GetType(index, &result.value);
    return result;
}

AsrResult IAsrSwigVariantVectorImpl::RemoveAt(size_t index)
{
    return impl_.RemoveAt(index);
}

AsrResult IAsrSwigVariantVectorImpl::GetSize() { return impl_.GetSize(); }

AsrResult IAsrVariantVectorImpl::QueryInterface(
    const AsrGuid& iid,
    void**         pp_object)
{
    return impl_.QueryInterface(iid, pp_object);
}

int64_t AsrVariantVectorImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t AsrVariantVectorImpl::Release() { return ref_counter_.Release(this); }

AsrResult AsrVariantVectorImpl::QueryInterface(
    const AsrGuid& iid,
    void**         pp_object)
{
    const auto cpp_qi_result = Utils::QueryInterface<IAsrVariantVector>(
        static_cast<IAsrVariantVectorImpl*>(*this),
        iid,
        pp_object);
    if (IsFailed(cpp_qi_result))
    {
        if (cpp_qi_result == ASR_E_NO_INTERFACE)
        {
            const auto swig_qi_result =
                Utils::QueryInterface<IAsrSwigVariantVector>(
                    static_cast<IAsrSwigVariantVectorImpl*>(*this),
                    iid,
                    pp_object);
            return swig_qi_result;
        }
        return cpp_qi_result;
    }
    return cpp_qi_result;
}

AsrResult AsrVariantVectorImpl::GetInt(size_t index, int64_t* p_out_int)
{
    ASR_UTILS_CHECK_POINTER(p_out_int)

    if (index < variants_.size())
    {
        return Details::Get(variants_[index], p_out_int);
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::GetFloat(size_t index, float* p_out_float)
{
    ASR_UTILS_CHECK_POINTER(p_out_float)

    if (index < variants_.size())
    {
        return Details::Get(variants_[index], p_out_float);
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::GetString(
    size_t               index,
    IAsrReadOnlyString** pp_out_string)
{
    ASR_UTILS_CHECK_POINTER(pp_out_string)

    if (index < variants_.size())
    {
        return Details::Get(variants_[index], pp_out_string);
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::GetBool(size_t index, bool* p_out_bool)
{
    ASR_UTILS_CHECK_POINTER(p_out_bool)

    if (index < variants_.size())
    {
        return Details::Get(variants_[index], p_out_bool);
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::GetComponent(
    size_t          index,
    IAsrComponent** pp_out_component)
{
    ASR_UTILS_CHECK_POINTER(pp_out_component)

    if (index < variants_.size())
    {
        return Details::Get2<IAsrComponent, IAsrSwigComponent>(
            variants_[index],
            pp_out_component);
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::GetBase(size_t index, IAsrBase** pp_out_base)
{
    ASR_UTILS_CHECK_POINTER(pp_out_base)

    if (index < variants_.size())
    {
        return Details::Get2<IAsrBase, IAsrSwigBase>(
            variants_[index],
            pp_out_base);
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrRetComponent AsrVariantVectorImpl::GetComponent(size_t index)
{
    if (index < variants_.size())
    {
        AsrRetComponent result;
        result.error_code = Details::Get2<IAsrSwigComponent, IAsrComponent>(
            variants_[index],
            result.value.Put());
        return result;
    }
    return {ASR_E_OUT_OF_RANGE};
}

AsrRetSwigBase AsrVariantVectorImpl::GetBase(size_t index)
{
    if (index < variants_.size())
    {
        AsrRetSwigBase result;
        IAsrSwigBase* p_base;
        result.error_code =
            Details::Get2<IAsrSwigBase, IAsrBase>(variants_[index], &p_base);
        result.value = p_base;
        return result;
    }
    return {ASR_E_OUT_OF_RANGE};
}

AsrResult AsrVariantVectorImpl::SetInt(size_t index, int64_t in_int)
{

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = in_int;
            return ASR_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            ASR_CORE_LOG_ERROR("Out of memory!");
            return ASR_E_OUT_OF_MEMORY;
        }
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::SetFloat(size_t index, float in_float)
{

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = in_float;
            return ASR_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            ASR_CORE_LOG_ERROR("Out of memory!");
            return ASR_E_OUT_OF_MEMORY;
        }
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::SetString(
    size_t              index,
    IAsrReadOnlyString* in_string)
{
    ASR_UTILS_CHECK_POINTER(in_string)

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = AsrReadOnlyString{in_string};
            return ASR_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            ASR_CORE_LOG_ERROR("Out of memory!");
            return ASR_E_OUT_OF_MEMORY;
        }
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::SetBool(size_t index, bool in_bool)
{

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = in_bool;
            return ASR_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            ASR_CORE_LOG_ERROR("Out of memory!");
            return ASR_E_OUT_OF_MEMORY;
        }
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::SetComponent(
    size_t         index,
    IAsrComponent* in_component)
{
    ASR_UTILS_CHECK_POINTER(in_component)

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = AsrPtr{in_component};
            return ASR_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            ASR_CORE_LOG_ERROR("Out of memory!");
            return ASR_E_OUT_OF_MEMORY;
        }
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::SetBase(size_t index, IAsrBase* in_base)
{
    ASR_UTILS_CHECK_POINTER(in_base)

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = AsrPtr{in_base};
            return ASR_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            ASR_CORE_LOG_ERROR("Out of memory!");
            return ASR_E_OUT_OF_MEMORY;
        }
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::SetComponent(
    size_t             index,
    IAsrSwigComponent* in_component)
{
    ASR_UTILS_CHECK_POINTER(in_component)

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = AsrPtr{in_component};
            return ASR_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            ASR_CORE_LOG_ERROR("Out of memory!");
            return ASR_E_OUT_OF_MEMORY;
        }
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::SetBase(size_t index, IAsrSwigBase* in_base)
{
    ASR_UTILS_CHECK_POINTER(in_base)

    if (index < variants_.size())
    {
        try
        {
            variants_[index] = AsrPtr{in_base};
            return ASR_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            ASR_CORE_LOG_ERROR("Out of memory!");
            return ASR_E_OUT_OF_MEMORY;
        }
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::PushBackInt(int64_t in_int)
{
    try
    {
        variants_.emplace_back(in_int);
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult AsrVariantVectorImpl::PushBackFloat(float in_float)
{
    try
    {
        variants_.emplace_back(in_float);
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult AsrVariantVectorImpl::PushBackString(IAsrReadOnlyString* in_string)
{
    ASR_UTILS_CHECK_POINTER(in_string)

    try
    {
        variants_.emplace_back(AsrReadOnlyString(in_string));
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult AsrVariantVectorImpl::PushBackBool(bool in_bool)
{
    try
    {
        variants_.emplace_back(in_bool);
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult AsrVariantVectorImpl::PushBackComponent(IAsrComponent* in_component)
{
    ASR_UTILS_CHECK_POINTER(in_component)

    try
    {
        variants_.emplace_back(AsrPtr{in_component});
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult AsrVariantVectorImpl::PushBackBase(IAsrBase* in_base)
{
    ASR_UTILS_CHECK_POINTER(in_base)

    try
    {
        variants_.emplace_back(AsrPtr{in_base});
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult AsrVariantVectorImpl::PushBackComponent(IAsrSwigComponent* in_component)
{
    ASR_UTILS_CHECK_POINTER(in_component)

    try
    {
        variants_.emplace_back(AsrPtr{in_component});
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult AsrVariantVectorImpl::PushBackBase(IAsrSwigBase* in_base)
{
    ASR_UTILS_CHECK_POINTER(in_base)

    try
    {
        variants_.emplace_back(AsrPtr{in_base});
        return ASR_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult AsrVariantVectorImpl::GetType(
    size_t          index,
    AsrVariantType* p_out_type)
{
    ASR_UTILS_CHECK_POINTER(p_out_type)

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
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::RemoveAt(size_t index)
{
    if (index < variants_.size())
    {
        variants_.erase(variants_.begin() + index);
        return ASR_S_OK;
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrResult AsrVariantVectorImpl::GetSize()
{
    const auto size = variants_.size();
    if (size > std::numeric_limits<AsrResult>::max())
    {
        ASR_CORE_LOG_ERROR("Overflow detected! Size = {}.", size);
    }
    return static_cast<AsrResult>(size);
}

AsrRetVariantType AsrVariantVectorImpl::ToType(const Variant& v)
{
    const auto i = v.index();
    switch (i)
    {
    case 0:
        return Details::MakeSuccess(ASR_VARIANT_TYPE_INT);
    case 1:
        return Details::MakeSuccess(ASR_VARIANT_TYPE_FLOAT);
    case 2:
        return Details::MakeSuccess(ASR_VARIANT_TYPE_BOOL);
    case 3:
        return Details::MakeSuccess(ASR_VARIANT_TYPE_STRING);
    case 4:
        [[fallthrough]];
    case 5:
        return Details::MakeSuccess(ASR_VARIANT_TYPE_BASE);
    case 6:
        [[fallthrough]];
    case 7:
        return Details::MakeSuccess(ASR_VARIANT_TYPE_COMPONENT);
    default:
        ASR_CORE_LOG_ERROR("Unexpected enum found. Value = {}", i);
        return {ASR_E_INTERNAL_FATAL_ERROR, ASR_VARIANT_TYPE_FORCE_DWORD};
    }
}

ASR_CORE_UTILS_NS_END
