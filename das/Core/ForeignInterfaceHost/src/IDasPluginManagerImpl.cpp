#include <das/Core/ForeignInterfaceHost/IDasPluginManagerImpl.h>
#include <das/Core/Logger/Logger.h>

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

DasPluginInfoImpl::DasPluginInfoImpl(std::shared_ptr<PluginPackageDesc> sp_desc)
    : sp_desc_{std::move(sp_desc)}
{
}

DAS_IMPL DasPluginInfoImpl::GetName(IDasReadOnlyString** pp_out_name)
{
    return GetStringImpl<&PluginPackageDesc::name>(pp_out_name);
}

DAS_IMPL DasPluginInfoImpl::GetDescription(
    IDasReadOnlyString** pp_out_description)
{
    return GetStringImpl<&PluginPackageDesc::description>(pp_out_description);
}

DAS_IMPL DasPluginInfoImpl::GetAuthor(IDasReadOnlyString** pp_out_author)
{
    return GetStringImpl<&PluginPackageDesc::author>(pp_out_author);
}

DAS_IMPL DasPluginInfoImpl::GetVersion(IDasReadOnlyString** pp_out_version)
{
    return GetStringImpl<&PluginPackageDesc::version>(pp_out_version);
}

DAS_IMPL DasPluginInfoImpl::GetSupportedSystem(
    IDasReadOnlyString** pp_out_supported_system)
{
    return GetStringImpl<&PluginPackageDesc::supported_system>(
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
        result = plugin_info_vector_[index].Get();
        result->AddRef();
        return DAS_S_OK;
    }
    return DAS_E_OUT_OF_RANGE;
}

void DasPluginInfoVectorImpl::AddInfo(DasPtr<DasPluginInfoImpl> sp_plugin_info)
{
    plugin_info_vector_.emplace_back(std::move(sp_plugin_info));
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
