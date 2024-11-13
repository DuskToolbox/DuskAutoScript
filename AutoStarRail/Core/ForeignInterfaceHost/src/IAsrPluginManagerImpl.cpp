#include <AutoStarRail/Core/ForeignInterfaceHost/IAsrPluginManagerImpl.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/Utils/QueryInterface.hpp>

IAsrPluginInfoImpl::IAsrPluginInfoImpl(AsrPluginInfoImpl& impl) : impl_{impl} {}

int64_t IAsrPluginInfoImpl::AddRef() { return impl_.AddRef(); }

int64_t IAsrPluginInfoImpl::Release() { return impl_.AddRef(); }

ASR_IMPL IAsrPluginInfoImpl::QueryInterface(
    const AsrGuid& iid,
    void**         pp_object)
{
    return ASR::Utils::QueryInterface<IAsrPluginInfo>(this, iid, pp_object);
}

ASR_IMPL IAsrPluginInfoImpl::GetName(IAsrReadOnlyString** pp_out_name)
{
    return impl_.GetName(pp_out_name);
}

ASR_IMPL IAsrPluginInfoImpl::GetDescription(
    IAsrReadOnlyString** pp_out_description)
{
    return impl_.GetName(pp_out_description);
}

ASR_IMPL IAsrPluginInfoImpl::GetAuthor(IAsrReadOnlyString** pp_out_author)
{
    return impl_.GetName(pp_out_author);
}

ASR_IMPL IAsrPluginInfoImpl::GetVersion(IAsrReadOnlyString** pp_out_version)
{
    return impl_.GetVersion(pp_out_version);
}

ASR_IMPL IAsrPluginInfoImpl::GetSupportedSystem(
    IAsrReadOnlyString** pp_out_supported_system)
{
    return impl_.GetSupportedSystem(pp_out_supported_system);
}

ASR_IMPL IAsrPluginInfoImpl::GetPluginIid(AsrGuid* p_out_guid)
{
    return impl_.GetPluginIid(p_out_guid);
}

AsrResult IAsrPluginInfoImpl::GetPluginSettingsDescriptor(
    IAsrReadOnlyString** pp_out_string)
{
    return impl_.GetPluginSettingsDescriptor(pp_out_string);
}

auto IAsrPluginInfoImpl::GetImpl() noexcept
    -> IAsrPluginInfoImpl::AsrPluginInfoImpl&
{
    return impl_;
}

IAsrSwigPluginInfoImpl::IAsrSwigPluginInfoImpl(AsrPluginInfoImpl& impl)
    : impl_{impl}
{
}

int64_t IAsrSwigPluginInfoImpl::AddRef() { return impl_.AddRef(); }

int64_t IAsrSwigPluginInfoImpl::Release() { return impl_.AddRef(); }

AsrRetSwigBase IAsrSwigPluginInfoImpl::QueryInterface(const AsrGuid& iid)
{
    return ASR::Utils::QueryInterface<IAsrSwigPluginInfo>(this, iid);
}

AsrRetReadOnlyString IAsrSwigPluginInfoImpl::GetName()
{
    return impl_.GetName();
}

AsrRetReadOnlyString IAsrSwigPluginInfoImpl::GetDescription()
{
    return impl_.GetDescription();
}

AsrRetReadOnlyString IAsrSwigPluginInfoImpl::GetAuthor()
{
    return impl_.GetAuthor();
}

AsrRetReadOnlyString IAsrSwigPluginInfoImpl::GetVersion()
{
    return impl_.GetVersion();
}

AsrRetReadOnlyString IAsrSwigPluginInfoImpl::GetSupportedSystem()
{
    return impl_.GetSupportedSystem();
}

AsrRetGuid IAsrSwigPluginInfoImpl::GetPluginIid()
{
    return impl_.GetPluginIid();
}

auto IAsrSwigPluginInfoImpl::GetImpl() noexcept
    -> IAsrSwigPluginInfoImpl::AsrPluginInfoImpl&
{
    return impl_;
}

IAsrPluginInfoVectorImpl::IAsrPluginInfoVectorImpl(
    IAsrPluginInfoVectorImpl::AsrPluginInfoVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IAsrPluginInfoVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IAsrPluginInfoVectorImpl::Release() { return impl_.AddRef(); }

ASR_IMPL IAsrPluginInfoVectorImpl::QueryInterface(
    const AsrGuid& iid,
    void**         pp_out_objects)
{
    return ASR::Utils::QueryInterface<IAsrPluginInfoVector>(
        this,
        iid,
        pp_out_objects);
}

ASR_IMPL IAsrPluginInfoVectorImpl::Size(size_t* p_out_size)
{
    return impl_.Size(p_out_size);
}

ASR_IMPL IAsrPluginInfoVectorImpl::At(
    size_t           index,
    IAsrPluginInfo** pp_out_info)
{
    return impl_.At(index, pp_out_info);
}

IAsrSwigPluginInfoVectorImpl::IAsrSwigPluginInfoVectorImpl(
    IAsrSwigPluginInfoVectorImpl::AsrPluginInfoVectorImpl& impl)
    : impl_{impl}
{
}

int64_t IAsrSwigPluginInfoVectorImpl::AddRef() { return impl_.AddRef(); }

int64_t IAsrSwigPluginInfoVectorImpl::Release() { return impl_.AddRef(); }

AsrRetSwigBase IAsrSwigPluginInfoVectorImpl::QueryInterface(const AsrGuid& iid)
{
    return ASR::Utils::QueryInterface<IAsrSwigPluginInfoVector>(this, iid);
}

AsrRetUInt IAsrSwigPluginInfoVectorImpl::Size() { return impl_.Size(); }

AsrRetPluginInfo IAsrSwigPluginInfoVectorImpl::At(size_t index)
{
    return impl_.At(index);
}

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

template <auto MemberPointer>
ASR_IMPL AsrPluginInfoImpl::GetStringImpl(IAsrReadOnlyString** pp_out_string)
{
    ASR_UTILS_CHECK_POINTER(pp_out_string)
    return ::CreateIAsrReadOnlyStringFromUtf8(
        (sp_desc_.get()->*MemberPointer).c_str(),
        pp_out_string);
}

template <auto MemberPointer>
AsrRetReadOnlyString AsrPluginInfoImpl::GetAsrStringImpl()
{
    ASR::AsrPtr<IAsrReadOnlyString> p_result;
    const auto error_code = ::CreateIAsrReadOnlyStringFromUtf8(
        (sp_desc_.get()->*MemberPointer).c_str(),
        p_result.Put());
    return {error_code, AsrReadOnlyString{std::move(p_result)}};
}

AsrPluginInfoImpl::AsrPluginInfoImpl(std::shared_ptr<PluginDesc> sp_desc)
    : ref_counter_{}, sp_desc_{sp_desc}, cpp_projection_{*this},
      swig_projection_{*this}
{
}

int64_t AsrPluginInfoImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t AsrPluginInfoImpl::Release() { return ref_counter_.Release(this); }

ASR_IMPL AsrPluginInfoImpl::GetName(IAsrReadOnlyString** pp_out_name)
{
    return GetStringImpl<&PluginDesc::name>(pp_out_name);
}

ASR_IMPL AsrPluginInfoImpl::GetDescription(
    IAsrReadOnlyString** pp_out_description)
{
    return GetStringImpl<&PluginDesc::description>(pp_out_description);
}

ASR_IMPL AsrPluginInfoImpl::GetAuthor(IAsrReadOnlyString** pp_out_author)
{
    return GetStringImpl<&PluginDesc::author>(pp_out_author);
}

ASR_IMPL AsrPluginInfoImpl::GetVersion(IAsrReadOnlyString** pp_out_version)
{
    return GetStringImpl<&PluginDesc::version>(pp_out_version);
}

ASR_IMPL AsrPluginInfoImpl::GetSupportedSystem(
    IAsrReadOnlyString** pp_out_supported_system)
{
    return GetStringImpl<&PluginDesc::supported_system>(
        pp_out_supported_system);
}

ASR_IMPL AsrPluginInfoImpl::GetPluginIid(AsrGuid* p_out_guid)
{
    ASR_UTILS_CHECK_POINTER(p_out_guid)
    *p_out_guid = sp_desc_->guid;
    return ASR_S_OK;
}

AsrResult AsrPluginInfoImpl::GetPluginSettingsDescriptor(
    IAsrReadOnlyString** pp_out_string)
{
    ASR_UTILS_CHECK_POINTER(pp_out_string)
    Utils::SetResult(sp_desc_->settings_desc_json.Get(), pp_out_string);
    return ASR_S_OK;
}

AsrRetReadOnlyString AsrPluginInfoImpl::GetName()
{
    return GetAsrStringImpl<&PluginDesc::name>();
}

AsrRetReadOnlyString AsrPluginInfoImpl::GetDescription()
{
    return GetAsrStringImpl<&PluginDesc::description>();
}

AsrRetReadOnlyString AsrPluginInfoImpl::GetAuthor()
{
    return GetAsrStringImpl<&PluginDesc::author>();
}

AsrRetReadOnlyString AsrPluginInfoImpl::GetVersion()
{
    return GetAsrStringImpl<&PluginDesc::version>();
}

AsrRetReadOnlyString AsrPluginInfoImpl::GetSupportedSystem()
{
    return GetAsrStringImpl<&PluginDesc::supported_system>();
}

AsrRetGuid AsrPluginInfoImpl::GetPluginIid()
{
    AsrRetGuid result{};
    result.value = sp_desc_->guid;
    result.error_code = ASR_S_OK;
    return result;
}

AsrPluginInfoImpl::operator IAsrPluginInfoImpl*() noexcept
{
    return &cpp_projection_;
}

AsrPluginInfoImpl::operator IAsrSwigPluginInfoImpl*() noexcept
{
    return &swig_projection_;
}

AsrPluginInfoImpl::operator AsrPtr<IAsrPluginInfoImpl>() noexcept
{
    return {&cpp_projection_};
}

AsrPluginInfoImpl::operator AsrPtr<IAsrSwigPluginInfoImpl>() noexcept
{
    return {&swig_projection_};
}

int64_t AsrPluginInfoVectorImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t AsrPluginInfoVectorImpl::Release()
{
    return ref_counter_.Release(this);
}

AsrResult AsrPluginInfoVectorImpl::Size(size_t* p_out_size)
{
    if (!p_out_size)
    {
        return ASR_E_INVALID_POINTER;
    }
    *p_out_size = plugin_info_vector_.size();
    return ASR_S_OK;
}

AsrResult AsrPluginInfoVectorImpl::At(
    size_t           index,
    IAsrPluginInfo** pp_out_info)
{
    if (index < plugin_info_vector_.size())
    {
        if (!pp_out_info)
        {
            return ASR_E_INVALID_POINTER;
        }
        auto& result = *pp_out_info;
        result = *plugin_info_vector_[index];
        result->AddRef();
        return ASR_S_OK;
    }
    return ASR_E_OUT_OF_RANGE;
}

AsrRetUInt AsrPluginInfoVectorImpl::Size()
{
    size_t     size{};
    const auto error_code = Size(&size);
    return {error_code, size};
}

AsrRetPluginInfo AsrPluginInfoVectorImpl::At(size_t index)
{
    AsrRetPluginInfo result{};
    if (index < plugin_info_vector_.size())
    {
        result.SetValue(*plugin_info_vector_[index]);
        result.error_code = ASR_S_OK;
        return result;
    }
    result.error_code = ASR_E_OUT_OF_RANGE;
    return result;
}

void AsrPluginInfoVectorImpl::AddInfo(
    std::unique_ptr<AsrPluginInfoImpl>&& up_plugin_info)
{
    plugin_info_vector_.emplace_back(std::move(up_plugin_info));
}

AsrPluginInfoVectorImpl::operator IAsrPluginInfoVectorImpl*() noexcept
{
    return &cpp_projection_;
}

AsrPluginInfoVectorImpl::operator IAsrSwigPluginInfoVectorImpl*() noexcept
{
    return &swig_projection_;
}

ASR_CORE_FOREIGNINTERFACEHOST_NS_END
