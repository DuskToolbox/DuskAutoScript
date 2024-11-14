#include <das/Core/ForeignInterfaceHost/IDasPluginManagerImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/QueryInterface.hpp>

IDasPluginInfoImpl::IDasPluginInfoImpl(DasPluginInfoImpl& impl) : impl_{impl} {}

int64_t IDasPluginInfoImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasPluginInfoImpl::Release() { return impl_.AddRef(); }

DAS_IMPL IDasPluginInfoImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    return DAS::Utils::QueryInterface<IDasPluginInfo>(this, iid, pp_object);
}

DAS_IMPL IDasPluginInfoImpl::GetName(IDasReadOnlyString** pp_out_name)
{
    return impl_.GetName(pp_out_name);
}

DAS_IMPL IDasPluginInfoImpl::GetDescription(
    IDasReadOnlyString** pp_out_description)
{
    return impl_.GetName(pp_out_description);
}

DAS_IMPL IDasPluginInfoImpl::GetAuthor(IDasReadOnlyString** pp_out_author)
{
    return impl_.GetName(pp_out_author);
}

DAS_IMPL IDasPluginInfoImpl::GetVersion(IDasReadOnlyString** pp_out_version)
{
    return impl_.GetVersion(pp_out_version);
}

DAS_IMPL IDasPluginInfoImpl::GetSupportedSystem(
    IDasReadOnlyString** pp_out_supported_system)
{
    return impl_.GetSupportedSystem(pp_out_supported_system);
}

DAS_IMPL IDasPluginInfoImpl::GetPluginIid(DasGuid* p_out_guid)
{
    return impl_.GetPluginIid(p_out_guid);
}

DasResult IDasPluginInfoImpl::GetPluginSettingsDescriptor(
    IDasReadOnlyString** pp_out_string)
{
    return impl_.GetPluginSettingsDescriptor(pp_out_string);
}

auto IDasPluginInfoImpl::GetImpl() noexcept
    -> IDasPluginInfoImpl::DasPluginInfoImpl&
{
    return impl_;
}

IDasSwigPluginInfoImpl::IDasSwigPluginInfoImpl(DasPluginInfoImpl& impl)
    : impl_{impl}
{
}

int64_t IDasSwigPluginInfoImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasSwigPluginInfoImpl::Release() { return impl_.AddRef(); }

DasRetSwigBase IDasSwigPluginInfoImpl::QueryInterface(const DasGuid& iid)
{
    return DAS::Utils::QueryInterface<IDasSwigPluginInfo>(this, iid);
}

DasRetReadOnlyString IDasSwigPluginInfoImpl::GetName()
{
    return impl_.GetName();
}

DasRetReadOnlyString IDasSwigPluginInfoImpl::GetDescription()
{
    return impl_.GetDescription();
}

DasRetReadOnlyString IDasSwigPluginInfoImpl::GetAuthor()
{
    return impl_.GetAuthor();
}

DasRetReadOnlyString IDasSwigPluginInfoImpl::GetVersion()
{
    return impl_.GetVersion();
}

DasRetReadOnlyString IDasSwigPluginInfoImpl::GetSupportedSystem()
{
    return impl_.GetSupportedSystem();
}

DasRetGuid IDasSwigPluginInfoImpl::GetPluginIid()
{
    return impl_.GetPluginIid();
}

auto IDasSwigPluginInfoImpl::GetImpl() noexcept
    -> IDasSwigPluginInfoImpl::DasPluginInfoImpl&
{
    return impl_;
}

IDasPluginInfoVectorImpl::IDasPluginInfoVectorImpl(
    IDasPluginInfoVectorImpl::DasPluginInfoVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IDasPluginInfoVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasPluginInfoVectorImpl::Release() { return impl_.AddRef(); }

DAS_IMPL IDasPluginInfoVectorImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_out_objects)
{
    return DAS::Utils::QueryInterface<IDasPluginInfoVector>(
        this,
        iid,
        pp_out_objects);
}

DAS_IMPL IDasPluginInfoVectorImpl::Size(size_t* p_out_size)
{
    return impl_.Size(p_out_size);
}

DAS_IMPL IDasPluginInfoVectorImpl::At(
    size_t           index,
    IDasPluginInfo** pp_out_info)
{
    return impl_.At(index, pp_out_info);
}

IDasSwigPluginInfoVectorImpl::IDasSwigPluginInfoVectorImpl(
    IDasSwigPluginInfoVectorImpl::DasPluginInfoVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IDasSwigPluginInfoVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasSwigPluginInfoVectorImpl::Release() { return impl_.AddRef(); }

DasRetSwigBase IDasSwigPluginInfoVectorImpl::QueryInterface(const DasGuid& iid)
{
    return DAS::Utils::QueryInterface<IDasSwigPluginInfoVector>(this, iid);
}

DasRetUInt IDasSwigPluginInfoVectorImpl::Size() { return impl_.Size(); }

DasRetPluginInfo IDasSwigPluginInfoVectorImpl::At(size_t index)
{
    return impl_.At(index);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

template <auto MemberPointer>
DAS_IMPL DasPluginInfoImpl::GetStringImpl(IDasReadOnlyString** pp_out_string)
{
    DAS_UTILS_CHECK_POINTER(pp_out_string)
    return ::CreateIDasReadOnlyStringFromUtf8(
        (sp_desc_.get()->*MemberPointer).c_str(),
        pp_out_string);
}

template <auto MemberPointer>
DasRetReadOnlyString DasPluginInfoImpl::GetDasStringImpl()
{
    DAS::DasPtr<IDasReadOnlyString> p_result;
    const auto error_code = ::CreateIDasReadOnlyStringFromUtf8(
        (sp_desc_.get()->*MemberPointer).c_str(),
        p_result.Put());
    return {error_code, DasReadOnlyString{std::move(p_result)}};
}

DasPluginInfoImpl::DasPluginInfoImpl(std::shared_ptr<PluginDesc> sp_desc)
    : ref_counter_{}, sp_desc_{sp_desc}, cpp_projection_{*this},
      swig_projection_{*this}
{
}

int64_t DasPluginInfoImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t DasPluginInfoImpl::Release() { return ref_counter_.Release(this); }

DAS_IMPL DasPluginInfoImpl::GetName(IDasReadOnlyString** pp_out_name)
{
    return GetStringImpl<&PluginDesc::name>(pp_out_name);
}

DAS_IMPL DasPluginInfoImpl::GetDescription(
    IDasReadOnlyString** pp_out_description)
{
    return GetStringImpl<&PluginDesc::description>(pp_out_description);
}

DAS_IMPL DasPluginInfoImpl::GetAuthor(IDasReadOnlyString** pp_out_author)
{
    return GetStringImpl<&PluginDesc::author>(pp_out_author);
}

DAS_IMPL DasPluginInfoImpl::GetVersion(IDasReadOnlyString** pp_out_version)
{
    return GetStringImpl<&PluginDesc::version>(pp_out_version);
}

DAS_IMPL DasPluginInfoImpl::GetSupportedSystem(
    IDasReadOnlyString** pp_out_supported_system)
{
    return GetStringImpl<&PluginDesc::supported_system>(
        pp_out_supported_system);
}

DAS_IMPL DasPluginInfoImpl::GetPluginIid(DasGuid* p_out_guid)
{
    DAS_UTILS_CHECK_POINTER(p_out_guid)
    *p_out_guid = sp_desc_->guid;
    return DAS_S_OK;
}

DasResult DasPluginInfoImpl::GetPluginSettingsDescriptor(
    IDasReadOnlyString** pp_out_string)
{
    DAS_UTILS_CHECK_POINTER(pp_out_string)
    Utils::SetResult(sp_desc_->settings_desc_json.Get(), pp_out_string);
    return DAS_S_OK;
}

DasRetReadOnlyString DasPluginInfoImpl::GetName()
{
    return GetDasStringImpl<&PluginDesc::name>();
}

DasRetReadOnlyString DasPluginInfoImpl::GetDescription()
{
    return GetDasStringImpl<&PluginDesc::description>();
}

DasRetReadOnlyString DasPluginInfoImpl::GetAuthor()
{
    return GetDasStringImpl<&PluginDesc::author>();
}

DasRetReadOnlyString DasPluginInfoImpl::GetVersion()
{
    return GetDasStringImpl<&PluginDesc::version>();
}

DasRetReadOnlyString DasPluginInfoImpl::GetSupportedSystem()
{
    return GetDasStringImpl<&PluginDesc::supported_system>();
}

DasRetGuid DasPluginInfoImpl::GetPluginIid()
{
    DasRetGuid result{};
    result.value = sp_desc_->guid;
    result.error_code = DAS_S_OK;
    return result;
}

DasPluginInfoImpl::operator IDasPluginInfoImpl*() noexcept
{
    return &cpp_projection_;
}

DasPluginInfoImpl::operator IDasSwigPluginInfoImpl*() noexcept
{
    return &swig_projection_;
}

DasPluginInfoImpl::operator DasPtr<IDasPluginInfoImpl>() noexcept
{
    return {&cpp_projection_};
}

DasPluginInfoImpl::operator DasPtr<IDasSwigPluginInfoImpl>() noexcept
{
    return {&swig_projection_};
}

int64_t DasPluginInfoVectorImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t DasPluginInfoVectorImpl::Release()
{
    return ref_counter_.Release(this);
}

DasResult DasPluginInfoVectorImpl::Size(size_t* p_out_size)
{
    if (!p_out_size)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_size = plugin_info_vector_.size();
    return DAS_S_OK;
}

DasResult DasPluginInfoVectorImpl::At(
    size_t           index,
    IDasPluginInfo** pp_out_info)
{
    if (index < plugin_info_vector_.size())
    {
        if (!pp_out_info)
        {
            return DAS_E_INVALID_POINTER;
        }
        auto& result = *pp_out_info;
        result = *plugin_info_vector_[index];
        result->AddRef();
        return DAS_S_OK;
    }
    return DAS_E_OUT_OF_RANGE;
}

DasRetUInt DasPluginInfoVectorImpl::Size()
{
    size_t     size{};
    const auto error_code = Size(&size);
    return {error_code, size};
}

DasRetPluginInfo DasPluginInfoVectorImpl::At(size_t index)
{
    DasRetPluginInfo result{};
    if (index < plugin_info_vector_.size())
    {
        result.SetValue(*plugin_info_vector_[index]);
        result.error_code = DAS_S_OK;
        return result;
    }
    result.error_code = DAS_E_OUT_OF_RANGE;
    return result;
}

void DasPluginInfoVectorImpl::AddInfo(
    std::unique_ptr<DasPluginInfoImpl>&& up_plugin_info)
{
    plugin_info_vector_.emplace_back(std::move(up_plugin_info));
}

DasPluginInfoVectorImpl::operator IDasPluginInfoVectorImpl*() noexcept
{
    return &cpp_projection_;
}

DasPluginInfoVectorImpl::operator IDasSwigPluginInfoVectorImpl*() noexcept
{
    return &swig_projection_;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
