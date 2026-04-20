#include <das/Core/ForeignInterfaceHost/IDasStringVectorImpl.h>

#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasStringVector.h>

#include <algorithm>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DasStringVectorImpl::DasStringVectorImpl(
    const std::vector<std::string>& strings)
    : strings_{strings}
{
}

uint32_t DasStringVectorImpl::AddRef() { return ++ref_count_; }

uint32_t DasStringVectorImpl::Release()
{
    const auto count = --ref_count_;
    if (count == 0)
    {
        delete this;
        return 0;
    }
    return count;
}

DasResult DasStringVectorImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_out_object)
{
    if (pp_out_object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    *pp_out_object = nullptr;

    if (iid == DasIidOf<IDasBase>())
    {
        *pp_out_object = static_cast<IDasBase*>(
            static_cast<ExportInterface::IDasStringVector*>(this));
    }
    else if (iid == DasIidOf<ExportInterface::IDasStringVector>())
    {
        *pp_out_object = static_cast<ExportInterface::IDasStringVector*>(this);
    }
    else
    {
        return DAS_E_NO_INTERFACE;
    }

    AddRef();
    return DAS_S_OK;
}

DasResult DasStringVectorImpl::Size(uint64_t* p_out_size)
{
    DAS_UTILS_CHECK_POINTER(p_out_size)

    *p_out_size = strings_.size();
    return DAS_S_OK;
}

DasResult DasStringVectorImpl::At(
    uint64_t             index,
    IDasReadOnlyString** pp_out_string)
{
    DAS_UTILS_CHECK_POINTER(pp_out_string)

    if (index >= strings_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    return CreateIDasReadOnlyStringFromUtf8(
        strings_[index].c_str(),
        pp_out_string);
}

DasResult DasStringVectorImpl::Find(IDasReadOnlyString* p_string)
{
    if (!p_string)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto expected_str = Utils::ToU8StringWithoutOwnership(p_string);
    if (!expected_str)
    {
        return expected_str.error();
    }

    const auto* target = expected_str.value();
    for (const auto& s : strings_)
    {
        if (s == target)
        {
            return DAS_S_OK;
        }
    }
    return DAS_E_OUT_OF_RANGE;
}

DasResult DasStringVectorImpl::PushBack(IDasReadOnlyString* p_string)
{
    if (!p_string)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto expected_str = Utils::ToU8StringWithoutOwnership(p_string);
    if (!expected_str)
    {
        return expected_str.error();
    }

    try
    {
        strings_.emplace_back(expected_str.value());
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    return DAS_S_OK;
}

auto DasStringVectorImpl::GetImpl() noexcept -> std::vector<std::string>&
{
    return strings_;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

DasResult
CreateIDasStringVector(Das::ExportInterface::IDasStringVector** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)

    try
    {
        auto* result =
            new DAS::Core::ForeignInterfaceHost::DasStringVectorImpl{};
        result->AddRef();
        *pp_out = result;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}
