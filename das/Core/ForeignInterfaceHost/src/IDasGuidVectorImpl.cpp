#include <das/Core/ForeignInterfaceHost/IDasGuidVectorImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/ExportInterface/IDasGuidVector.h>
#include <das/Utils/QueryInterface.hpp>
#include <algorithm>

IDasReadOnlyGuidVectorImpl::IDasReadOnlyGuidVectorImpl(
    Das::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IDasReadOnlyGuidVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasReadOnlyGuidVectorImpl::Release() { return impl_.Release(); }

DasResult IDasReadOnlyGuidVectorImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_out_object)
{
    return DAS::Utils::QueryInterface<IDasReadOnlyGuidVector>(
        this,
        iid,
        pp_out_object);
}

DasResult IDasReadOnlyGuidVectorImpl::Size(size_t* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size);

    *p_out_size = impl_.Size();

    return DAS_S_OK;
}

DasResult IDasReadOnlyGuidVectorImpl::At(size_t index, DasGuid* p_out_iid)
{
    DAS_UTILS_CHECK_POINTER(p_out_iid)

    return impl_.At(index, *p_out_iid);
}

DasResult IDasReadOnlyGuidVectorImpl::Find(const DasGuid& iid)
{
    return impl_.Find(iid);
}

IDasGuidVectorImpl::IDasGuidVectorImpl(
    Das::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IDasGuidVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasGuidVectorImpl::Release() { return impl_.Release(); }

DasResult IDasGuidVectorImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_out_object)
{
    return DAS::Utils::QueryInterface<IDasGuidVector>(this, iid, pp_out_object);
}

DasResult IDasGuidVectorImpl::Size(size_t* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size);

    *p_out_size = impl_.Size();

    return DAS_S_OK;
}

DasResult IDasGuidVectorImpl::At(size_t index, DasGuid* p_out_iid)
{
    DAS_UTILS_CHECK_POINTER(p_out_iid)

    return impl_.At(index, *p_out_iid);
}

DasResult IDasGuidVectorImpl::Find(const DasGuid& iid)
{
    return impl_.Find(iid);
}

DasResult IDasGuidVectorImpl::PushBack(const DasGuid& iid)
{
    return impl_.PushBack(iid);
}

DasResult IDasGuidVectorImpl::ToConst(IDasReadOnlyGuidVector** pp_out_object)
{
    DAS_UTILS_CHECK_POINTER(pp_out_object)

    auto expected_p_impl = impl_.ToConst();
    if (!expected_p_impl)
    {
        return expected_p_impl.error();
    }
    const auto p_result =
        DAS::MakeDasPtr<IDasReadOnlyGuidVector, IDasReadOnlyGuidVectorImpl>(
            *expected_p_impl.value());
    *pp_out_object = p_result.Get();
    p_result->AddRef();

    return DAS_S_OK;
}

auto IDasGuidVectorImpl::GetImpl() noexcept -> std::vector<DasGuid>&
{
    return impl_.GetImpl();
}

auto IDasGuidVectorImpl::Get()
    -> Das::Core::ForeignInterfaceHost::DasGuidVectorImpl&
{
    return impl_;
}
IDasSwigReadOnlyGuidVectorImpl::IDasSwigReadOnlyGuidVectorImpl(
    Das::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IDasSwigReadOnlyGuidVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasSwigReadOnlyGuidVectorImpl::Release() { return impl_.Release(); }

DasRetSwigBase IDasSwigReadOnlyGuidVectorImpl::QueryInterface(
    const DasGuid& iid)
{
    return DAS::Utils::QueryInterface<IDasSwigReadOnlyGuidVector>(this, iid);
}

DasRetUInt IDasSwigReadOnlyGuidVectorImpl::Size()
{
    return {DAS_S_OK, impl_.Size()};
}

DasRetGuid IDasSwigReadOnlyGuidVectorImpl::At(size_t index)
{
    DasRetGuid result{};

    result.error_code = impl_.At(index, result.value);

    return result;
}

DasResult IDasSwigReadOnlyGuidVectorImpl::Find(const DasGuid& iid)
{
    return impl_.Find(iid);
}

IDasSwigGuidVectorImpl::IDasSwigGuidVectorImpl(
    Das::Core::ForeignInterfaceHost::DasGuidVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IDasSwigGuidVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasSwigGuidVectorImpl::Release() { return impl_.Release(); }

DasRetSwigBase IDasSwigGuidVectorImpl::QueryInterface(const DasGuid& iid)
{
    return DAS::Utils::QueryInterface<IDasSwigGuidVector>(this, iid);
}

DasRetUInt IDasSwigGuidVectorImpl::Size() { return {DAS_S_OK, impl_.Size()}; }

DasRetGuid IDasSwigGuidVectorImpl::At(size_t index)
{
    DasRetGuid result{};

    result.error_code = impl_.At(index, result.value);

    return result;
}

DasResult IDasSwigGuidVectorImpl::Find(const DasGuid& iid)
{
    return impl_.Find(iid);
}

DasResult IDasSwigGuidVectorImpl::PushBack(const DasGuid& iid)
{
    return impl_.PushBack(iid);
}

DasRetReadOnlyGuidVector IDasSwigGuidVectorImpl::ToConst()
{
    auto expected_p_impl = impl_.ToConst();
    if (!expected_p_impl)
    {
        return {expected_p_impl.error()};
    }
    return {
        DAS_S_OK,
        DAS::MakeDasPtr<
            IDasSwigReadOnlyGuidVector,
            IDasSwigReadOnlyGuidVectorImpl>(*expected_p_impl.value())};
}

auto IDasSwigGuidVectorImpl::Get()
    -> Das::Core::ForeignInterfaceHost::DasGuidVectorImpl&
{
    return impl_;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DasGuidVectorImpl::DasGuidVectorImpl(const std::vector<DasGuid>& iids)
    : ref_counter_{}, iids_{iids}
{
}

int64_t DasGuidVectorImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t DasGuidVectorImpl::Release() { return ref_counter_.Release(this); }

auto DasGuidVectorImpl::Size() const noexcept -> size_t { return iids_.size(); }

auto DasGuidVectorImpl::At(size_t index, DasGuid& out_guid) const noexcept
    -> DasResult
{
    if (index >= iids_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    out_guid = iids_[index];

    return DAS_S_OK;
}

auto DasGuidVectorImpl::Find(const DasGuid guid) noexcept -> DasResult
{
    const auto it_guid = std::find(DAS_FULL_RANGE_OF(iids_), guid);
    return it_guid != iids_.end() ? DAS_S_OK : DAS_E_OUT_OF_RANGE;
}

auto DasGuidVectorImpl::PushBack(const DasGuid guid) noexcept -> DasResult
{
    try
    {
        iids_.push_back(guid);
    }
    catch (std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
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
    const DasGuid*   p_iids,
    size_t           iid_count,
    IDasGuidVector** pp_out_iid_vector)
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

        *pp_out_iid_vector = *result;
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

DasRetGuidVector CreateIDasSwigGuidVector()
{
    DasRetGuidVector result{};
    try
    {
        const auto p_result =
            new DAS::Core::ForeignInterfaceHost::DasGuidVectorImpl{};
        result.value = {static_cast<IDasSwigGuidVector*>(*p_result)};
    }
    catch (const std::bad_alloc&)
    {
        result.error_code = DAS_E_OUT_OF_MEMORY;
    }
    return result;
}
