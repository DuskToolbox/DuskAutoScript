#include <algorithm>
#include <das/Core/ForeignInterfaceHost/IDasInputFactoryVectorImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>
#include <ranges>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

auto DasInputFactoryVectorImpl::InternalFind(const DasGuid& iid)
    -> DasInputFactoryVectorImpl::ContainerIt
{
    return std::ranges::find_if(
        input_factory_vector_,
        [iid](const auto& factory)
        {
            const auto ret_guid = factory
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

// Note: AddRef(), Release(), QueryInterface() are now provided by
// DasInputFactoryVectorImplBase

DasResult DasInputFactoryVectorImpl::Size(size_t* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size);

    *p_out_size = input_factory_vector_.size();
    return DAS_S_OK;
}

DasResult DasInputFactoryVectorImpl::At(
    size_t             index,
    IDasInputFactory** pp_out_factory)
{
    if (index < input_factory_vector_.size())
    {
        DAS_UTILS_CHECK_POINTER(pp_out_factory)

        *pp_out_factory = input_factory_vector_[index].Get();
        (*pp_out_factory)->AddRef();
        return DAS_S_OK;
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasInputFactoryVectorImpl::Find(
    const DasGuid&     iid,
    IDasInputFactory** pp_out_factory)
{
    const auto it = InternalFind(iid);
    if (it != input_factory_vector_.end())
    {
        DAS_UTILS_CHECK_POINTER(pp_out_factory)

        const auto p_result = it->Get();
        *pp_out_factory = p_result;
        p_result->AddRef();
        return DAS_S_OK;
    }
    return DAS_E_OUT_OF_RANGE;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
