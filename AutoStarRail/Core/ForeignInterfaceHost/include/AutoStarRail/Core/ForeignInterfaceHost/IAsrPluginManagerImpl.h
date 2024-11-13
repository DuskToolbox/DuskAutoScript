#ifndef ASR_CORE_FOREIGNINTERFACEHOST_IASRPLUGINMANAGERIMPL_H
#define ASR_CORE_FOREIGNINTERFACEHOST_IASRPLUGINMANAGERIMPL_H

#include "ForeignInterfaceHost.h"

#include <AutoStarRail/Core/ForeignInterfaceHost/Config.h>
#include <AutoStarRail/ExportInterface/IAsrPluginManager.h>
#include <AutoStarRail/Utils/CommonUtils.hpp>
#include <vector>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class AsrPluginInfoImpl;
class AsrPluginInfoVectorImpl;

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

class IAsrPluginInfoImpl final : public IAsrPluginInfo
{
    using AsrPluginInfoImpl =
        ASR::Core::ForeignInterfaceHost::AsrPluginInfoImpl;
    AsrPluginInfoImpl& impl_;

public:
    IAsrPluginInfoImpl(AsrPluginInfoImpl& impl);
    // IAsrBase
    int64_t  AddRef() override;
    int64_t  Release() override;
    ASR_IMPL QueryInterface(const AsrGuid& iid, void** pp_object) override;
    // IAsrPluginInfo
    ASR_IMPL GetName(IAsrReadOnlyString** pp_out_name) override;
    ASR_IMPL GetDescription(IAsrReadOnlyString** pp_out_description) override;
    ASR_IMPL GetAuthor(IAsrReadOnlyString** pp_out_author) override;
    ASR_IMPL GetVersion(IAsrReadOnlyString** pp_out_version) override;
    ASR_IMPL GetSupportedSystem(
        IAsrReadOnlyString** pp_out_supported_system) override;
    ASR_IMPL GetPluginIid(AsrGuid* p_out_guid) override;
    ASR_IMPL GetPluginSettingsDescriptor(
        IAsrReadOnlyString** pp_out_string) override;

    auto GetImpl() noexcept -> AsrPluginInfoImpl&;
};

class IAsrSwigPluginInfoImpl final : public IAsrSwigPluginInfo
{
    using AsrPluginInfoImpl =
        ASR::Core::ForeignInterfaceHost::AsrPluginInfoImpl;
    AsrPluginInfoImpl& impl_;

public:
    IAsrSwigPluginInfoImpl(AsrPluginInfoImpl& impl);
    // IAsrSwigBase
    int64_t        AddRef() override;
    int64_t        Release() override;
    AsrRetSwigBase QueryInterface(const AsrGuid& iid) override;
    // IAsrSwigPluginInfo
    AsrRetReadOnlyString GetName() override;
    AsrRetReadOnlyString GetDescription() override;
    AsrRetReadOnlyString GetAuthor() override;
    AsrRetReadOnlyString GetVersion() override;
    AsrRetReadOnlyString GetSupportedSystem() override;
    AsrRetGuid           GetPluginIid() override;

    auto GetImpl() noexcept -> AsrPluginInfoImpl&;
};

class IAsrPluginInfoVectorImpl final : public IAsrPluginInfoVector
{
    using AsrPluginInfoVectorImpl =
        ASR::Core::ForeignInterfaceHost::AsrPluginInfoVectorImpl;
    AsrPluginInfoVectorImpl& impl_;

public:
    IAsrPluginInfoVectorImpl(AsrPluginInfoVectorImpl& impl);
    // IAsrBase
    int64_t  AddRef() override;
    int64_t  Release() override;
    ASR_IMPL QueryInterface(const AsrGuid& iid, void** pp_out_objects) override;
    // IAsrPluginInfoVector
    ASR_IMPL Size(size_t* p_out_size) override;
    ASR_IMPL At(size_t index, IAsrPluginInfo** pp_out_info) override;
};

class IAsrSwigPluginInfoVectorImpl final : public IAsrSwigPluginInfoVector
{
    using AsrPluginInfoVectorImpl =
        ASR::Core::ForeignInterfaceHost::AsrPluginInfoVectorImpl;
    AsrPluginInfoVectorImpl& impl_;

public:
    IAsrSwigPluginInfoVectorImpl(AsrPluginInfoVectorImpl& impl);
    // IAsrSwigBase
    int64_t        AddRef() override;
    int64_t        Release() override;
    AsrRetSwigBase QueryInterface(const AsrGuid& iid) override;
    // IAsrSwigPluginInfoVector
    AsrRetUInt       Size() override;
    AsrRetPluginInfo At(size_t index) override;
};

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class AsrPluginInfoImpl
{
    ASR::Utils::RefCounter<AsrPluginInfoImpl> ref_counter_;
    std::shared_ptr<PluginDesc>               sp_desc_;
    IAsrPluginInfoImpl                        cpp_projection_;
    IAsrSwigPluginInfoImpl                    swig_projection_;

    template <auto MemberPointer>
    ASR_IMPL GetStringImpl(IAsrReadOnlyString** pp_out_string);
    template <auto MemberPointer>
    AsrRetReadOnlyString GetAsrStringImpl();

public:
    AsrPluginInfoImpl(std::shared_ptr<PluginDesc> sp_desc);

    int64_t AddRef();
    int64_t Release();

    AsrResult GetName(IAsrReadOnlyString** pp_out_name);
    AsrResult GetDescription(IAsrReadOnlyString** pp_out_description);
    AsrResult GetAuthor(IAsrReadOnlyString** pp_out_author);
    AsrResult GetVersion(IAsrReadOnlyString** pp_out_version);
    AsrResult GetSupportedSystem(IAsrReadOnlyString** pp_out_supported_system);
    AsrResult GetPluginIid(AsrGuid* p_out_guid);
    AsrResult GetPluginSettingsDescriptor(IAsrReadOnlyString** pp_out_string);
    AsrRetReadOnlyString GetName();
    AsrRetReadOnlyString GetDescription();
    AsrRetReadOnlyString GetAuthor();
    AsrRetReadOnlyString GetVersion();
    AsrRetReadOnlyString GetSupportedSystem();
    AsrRetGuid           GetPluginIid();

    operator IAsrPluginInfoImpl*() noexcept;
    operator IAsrSwigPluginInfoImpl*() noexcept;
    explicit operator AsrPtr<IAsrPluginInfoImpl>() noexcept;
    explicit operator AsrPtr<IAsrSwigPluginInfoImpl>() noexcept;
};

class AsrPluginInfoVectorImpl
{
    ASR::Utils::RefCounter<AsrPluginInfoVectorImpl> ref_counter_{};
    std::vector<std::unique_ptr<AsrPluginInfoImpl>> plugin_info_vector_{};
    IAsrPluginInfoVectorImpl                        cpp_projection_{*this};
    IAsrSwigPluginInfoVectorImpl                    swig_projection_{*this};

public:
    int64_t AddRef();
    int64_t Release();

    AsrResult        Size(size_t* p_out_size);
    AsrResult        At(size_t index, IAsrPluginInfo** pp_out_info);
    AsrRetUInt       Size();
    AsrRetPluginInfo At(size_t index);

    void AddInfo(std::unique_ptr<AsrPluginInfoImpl>&& up_plugin_info);

    operator IAsrPluginInfoVectorImpl*() noexcept;
    operator IAsrSwigPluginInfoVectorImpl*() noexcept;
};

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // ASR_CORE_FOREIGNINTERFACEHOST_IASRPLUGINMANAGERIMPL_H
