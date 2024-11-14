#include <das/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <das/Core/ForeignInterfaceHost/IDasInputFactoryVectorImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/QueryInterface.hpp>
#include <algorithm>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

IDasInputFactoryVectorImpl::IDasInputFactoryVectorImpl(
    DasInputFactoryVectorImpl& impl)
    : impl_{impl}
{
}

auto IDasInputFactoryVectorImpl::AddRef() -> int64_t { return impl_.AddRef(); }

auto IDasInputFactoryVectorImpl::Release() -> int64_t
{
    return impl_.Release();
}

auto IDasInputFactoryVectorImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_object) -> DasResult
{
    return Utils::QueryInterface<IDasInputFactoryVector>(this, iid, pp_object);
}

DasResult IDasInputFactoryVectorImpl::Size(size_t* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size);

    *p_out_size = impl_.Size();
    return DAS_S_OK;
}

DasResult IDasInputFactoryVectorImpl::At(
    size_t             index,
    IDasInputFactory** pp_out_factory)
{
    return impl_.At(index, pp_out_factory);
}

DasResult IDasInputFactoryVectorImpl::Find(
    const DasGuid&     iid,
    IDasInputFactory** pp_out_factory)
{
    return impl_.Find(iid, pp_out_factory);
}

IDasSwigInputFactoryVectorImpl::IDasSwigInputFactoryVectorImpl(
    DasInputFactoryVectorImpl& impl)
    : impl_{impl}
{
}

auto IDasSwigInputFactoryVectorImpl::AddRef() -> int64_t
{
    return impl_.AddRef();
}

auto IDasSwigInputFactoryVectorImpl::Release() -> int64_t
{
    return impl_.Release();
}

auto IDasSwigInputFactoryVectorImpl::Size() -> DasRetUInt
{
    return {DAS_S_OK, impl_.Size()};
}

auto IDasSwigInputFactoryVectorImpl::At(size_t index) -> DasRetInputFactory
{
    DasRetInputFactory       result{};
    DasPtr<IDasInputFactory> p_cpp_result;

    result.error_code = impl_.At(index, p_cpp_result.Put());
    if (IsFailed(result.error_code))
    {
        return result;
    }

    const auto expected_result =
        MakeInterop<IDasSwigInputFactory>(p_cpp_result.Get());
    ToDasRetType(expected_result, result);
    return result;
}

auto IDasSwigInputFactoryVectorImpl::Find(const DasGuid& iid)
    -> DasRetInputFactory
{
    DasRetInputFactory       result{};
    DasPtr<IDasInputFactory> p_cpp_result{};

    result.error_code = impl_.Find(iid, p_cpp_result.Put());
    if (IsFailed(result.error_code))
    {
        return result;
    }

    const auto p_result = MakeInterop<IDasSwigInputFactory>(p_cpp_result.Get());
    ToDasRetType(p_result, result);
    return result;
}
auto IDasSwigInputFactoryVectorImpl::QueryInterface(const DasGuid& iid)
    -> DasRetSwigBase
{
    return Utils::QueryInterface<IDasSwigInputFactoryVector>(this, iid);
}

auto DasInputFactoryVectorImpl::InternalFind(const DasGuid& iid)
    -> DasInputFactoryVectorImpl::ContainerIt
{
    return std::ranges::find_if(
        input_factory_vector_,
        [iid](const auto& pair)
        {
            const auto ret_guid = pair.second->GetGuid();
            if (IsFailed(ret_guid.error_code))
            {
                return false;
            }
            return ret_guid.value == iid;
        });
}

DasInputFactoryVectorImpl::DasInputFactoryVectorImpl(
    const InputFactoryManager& manager)
    : input_factory_vector_{manager.GetVector()}
{
}

int64_t DasInputFactoryVectorImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t DasInputFactoryVectorImpl::Release()
{
    return ref_counter_.Release(this);
}

auto DasInputFactoryVectorImpl::Size() const noexcept -> size_t
{
    return input_factory_vector_.size();
}

auto DasInputFactoryVectorImpl::At(
    size_t             index,
    IDasInputFactory** pp_out_factory) const -> DasResult
{
    if (index < input_factory_vector_.size())
    {
        DAS_UTILS_CHECK_POINTER(pp_out_factory)

        *pp_out_factory = input_factory_vector_[index].first.Get();
        (*pp_out_factory)->AddRef();
        return DAS_S_OK;
    }
    return DAS_E_OUT_OF_RANGE;
}

auto DasInputFactoryVectorImpl::Find(
    const DasGuid&     iid,
    IDasInputFactory** pp_out_factory) -> DasResult
{
    const auto it = InternalFind(iid);
    if (it != input_factory_vector_.end())
    {
        DAS_UTILS_CHECK_POINTER(pp_out_factory)

        const auto p_result = it->first.Get();
        *pp_out_factory = p_result;
        p_result->AddRef();
        return DAS_S_OK;
    }
    return DAS_E_OUT_OF_RANGE;
}

auto DasInputFactoryVectorImpl::At(size_t index) -> DasRetInputFactory
{
    if (index < input_factory_vector_.size())
    {
        const auto p_result = input_factory_vector_[index].second.Get();
        return {DAS_S_OK, p_result};
    }
    return {DAS_E_OUT_OF_RANGE, nullptr};
}

auto DasInputFactoryVectorImpl::Find(const DasGuid& iid) -> DasRetInputFactory
{
    const auto it = InternalFind(iid);
    if (it != input_factory_vector_.end())
    {
        const auto p_result = it->second.Get();
        return {DAS_S_OK, p_result};
    }
    return {DAS_E_OUT_OF_RANGE, nullptr};
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
