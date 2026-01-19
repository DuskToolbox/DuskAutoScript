#include <DAS/_autogen/idl/abi/IDasGuidVector.h>
#include <algorithm>
#include <das/Core/ForeignInterfaceHost/IDasGuidVectorImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DasGuidVectorImpl::DasGuidVectorImpl(const std::vector<DasGuid>& iids)
    : iids_{iids}
{
}

// Note: AddRef(), Release(), QueryInterface() are now provided by
// DasGuidVectorImplBase

DasResult DasGuidVectorImpl::Size(size_t* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size);

    *p_out_size = iids_.size();
    return DAS_S_OK;
}

DasResult DasGuidVectorImpl::At(size_t index, DasGuid* p_out_iid)
{
    DAS_UTILS_CHECK_POINTER(p_out_iid);

    if (index >= iids_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    *p_out_iid = iids_[index];
    return DAS_S_OK;
}

DasResult DasGuidVectorImpl::Find(const DasGuid& iid)
{
    const auto it_guid = std::find(DAS_FULL_RANGE_OF(iids_), iid);
    return it_guid != iids_.end() ? DAS_S_OK : DAS_E_OUT_OF_RANGE;
}

DasResult DasGuidVectorImpl::PushBack(const DasGuid& iid)
{
    try
    {
        iids_.push_back(iid);
    }
    catch (std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    return DAS_S_OK;
}

DasResult DasGuidVectorImpl::ToConst(
    ExportInterface::IDasReadOnlyGuidVector** pp_out_object)
{
    DAS_UTILS_CHECK_POINTER(pp_out_object);

    auto expected_p_impl = ToConst();
    if (!expected_p_impl)
    {
        return expected_p_impl.error();
    }
    *pp_out_object = expected_p_impl.value().Get();
    (*pp_out_object)->AddRef();
    return DAS_S_OK;
}

auto DasGuidVectorImpl::GetImpl() noexcept -> std::vector<DasGuid>&
{
    return iids_;
}

auto DasGuidVectorImpl::ToConst() noexcept
    -> Utils::Expected<DasPtr<DasGuidVectorImpl>>
{
    try
    {
        return MakeDasPtr<DasGuidVectorImpl>(iids_);
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return tl::make_unexpected(DAS_E_OUT_OF_MEMORY);
    }
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

DasResult CreateIDasGuidVector(
    const DasGuid*                         p_iids,
    size_t                                 iid_count,
    Das::ExportInterface::IDasGuidVector** pp_out_iid_vector)
{
    *pp_out_iid_vector = nullptr;
    try
    {
        auto* const result =
            new DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl{};
        result->AddRef();

        if (iid_count > 0)
        {
            auto& impl = result->GetImpl();
            impl.resize(iid_count);
            DAS_UTILS_CHECK_POINTER(p_iids)
            DAS::Utils::CopyArray(p_iids, iid_count, impl.data());
        }

        *pp_out_iid_vector = result->ToConst();
        return DAS_S_OK;
    }
    catch (std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (std::length_error&)
    {
        DAS_CORE_LOG_ERROR(
            "Error happened because iids_count > iids_.max_size().]\nNOTE: iid_count = {}.",
            iid_count);
        return DAS_E_OUT_OF_MEMORY;
    }
}
