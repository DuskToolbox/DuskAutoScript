#ifndef DAS_CORE_FOREIGNINTERFACEHOST_IDASPLUGINMANAGERIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_IDASPLUGINMANAGERIMPL_H

#include "ForeignInterfaceHost.h"

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/ExportInterface/IDasPluginManager.h>
#include <das/Utils/CommonUtils.hpp>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class DasPluginInfoImpl;
class DasPluginInfoVectorImpl;

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

class IDasPluginInfoImpl final : public IDasPluginInfo
{
    using DasPluginInfoImpl =
        DAS::Core::ForeignInterfaceHost::DasPluginInfoImpl;
    DasPluginInfoImpl& impl_;

public:
    IDasPluginInfoImpl(DasPluginInfoImpl& impl);
    // IDasBase
    int64_t  AddRef() override;
    int64_t  Release() override;
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_object) override;
    // IDasPluginInfo
    DAS_IMPL GetName(IDasReadOnlyString** pp_out_name) override;
    DAS_IMPL GetDescription(IDasReadOnlyString** pp_out_description) override;
    DAS_IMPL GetAuthor(IDasReadOnlyString** pp_out_author) override;
    DAS_IMPL GetVersion(IDasReadOnlyString** pp_out_version) override;
    DAS_IMPL GetSupportedSystem(
        IDasReadOnlyString** pp_out_supported_system) override;
    DAS_IMPL GetPluginIid(DasGuid* p_out_guid) override;
    DAS_IMPL GetPluginSettingsDescriptor(
        IDasReadOnlyString** pp_out_string) override;

    auto GetImpl() noexcept -> DasPluginInfoImpl&;
};

class IDasSwigPluginInfoImpl final : public IDasSwigPluginInfo
{
    using DasPluginInfoImpl =
        DAS::Core::ForeignInterfaceHost::DasPluginInfoImpl;
    DasPluginInfoImpl& impl_;

public:
    IDasSwigPluginInfoImpl(DasPluginInfoImpl& impl);
    // IDasSwigBase
    int64_t        AddRef() override;
    int64_t        Release() override;
    DasRetSwigBase QueryInterface(const DasGuid& iid) override;
    // IDasSwigPluginInfo
    DasRetReadOnlyString GetName() override;
    DasRetReadOnlyString GetDescription() override;
    DasRetReadOnlyString GetAuthor() override;
    DasRetReadOnlyString GetVersion() override;
    DasRetReadOnlyString GetSupportedSystem() override;
    DasRetGuid           GetPluginIid() override;

    auto GetImpl() noexcept -> DasPluginInfoImpl&;
};

class IDasPluginInfoVectorImpl final : public IDasPluginInfoVector
{
    using DasPluginInfoVectorImpl =
        DAS::Core::ForeignInterfaceHost::DasPluginInfoVectorImpl;
    DasPluginInfoVectorImpl& impl_;

public:
    IDasPluginInfoVectorImpl(DasPluginInfoVectorImpl& impl);
    // IDasBase
    int64_t  AddRef() override;
    int64_t  Release() override;
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_out_objects) override;
    // IDasPluginInfoVector
    DAS_IMPL Size(size_t* p_out_size) override;
    DAS_IMPL At(size_t index, IDasPluginInfo** pp_out_info) override;
};

class IDasSwigPluginInfoVectorImpl final : public IDasSwigPluginInfoVector
{
    using DasPluginInfoVectorImpl =
        DAS::Core::ForeignInterfaceHost::DasPluginInfoVectorImpl;
    DasPluginInfoVectorImpl& impl_;

public:
    IDasSwigPluginInfoVectorImpl(DasPluginInfoVectorImpl& impl);
    // IDasSwigBase
    int64_t        AddRef() override;
    int64_t        Release() override;
    DasRetSwigBase QueryInterface(const DasGuid& iid) override;
    // IDasSwigPluginInfoVector
    DasRetUInt       Size() override;
    DasRetPluginInfo At(size_t index) override;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class DasPluginInfoImpl
{
    DAS::Utils::RefCounter<DasPluginInfoImpl> ref_counter_;
    std::shared_ptr<PluginPackageDesc>               sp_desc_;
    IDasPluginInfoImpl                        cpp_projection_;
    IDasSwigPluginInfoImpl                    swig_projection_;

    template <auto MemberPointer>
    DAS_IMPL GetStringImpl(IDasReadOnlyString** pp_out_string);
    template <auto MemberPointer>
    DasRetReadOnlyString GetDasStringImpl();

public:
    DasPluginInfoImpl(std::shared_ptr<PluginPackageDesc> sp_desc);

    int64_t AddRef();
    int64_t Release();

    DasResult GetName(IDasReadOnlyString** pp_out_name);
    DasResult GetDescription(IDasReadOnlyString** pp_out_description);
    DasResult GetAuthor(IDasReadOnlyString** pp_out_author);
    DasResult GetVersion(IDasReadOnlyString** pp_out_version);
    DasResult GetSupportedSystem(IDasReadOnlyString** pp_out_supported_system);
    DasResult GetPluginIid(DasGuid* p_out_guid);
    DasResult GetPluginSettingsDescriptor(IDasReadOnlyString** pp_out_string);
    DasRetReadOnlyString GetName();
    DasRetReadOnlyString GetDescription();
    DasRetReadOnlyString GetAuthor();
    DasRetReadOnlyString GetVersion();
    DasRetReadOnlyString GetSupportedSystem();
    DasRetGuid           GetPluginIid();

    operator IDasPluginInfoImpl*() noexcept;
    operator IDasSwigPluginInfoImpl*() noexcept;
    explicit operator DasPtr<IDasPluginInfoImpl>() noexcept;
    explicit operator DasPtr<IDasSwigPluginInfoImpl>() noexcept;
};

class DasPluginInfoVectorImpl
{
    DAS::Utils::RefCounter<DasPluginInfoVectorImpl> ref_counter_{};
    std::vector<std::unique_ptr<DasPluginInfoImpl>> plugin_info_vector_{};
    IDasPluginInfoVectorImpl                        cpp_projection_{*this};
    IDasSwigPluginInfoVectorImpl                    swig_projection_{*this};

public:
    int64_t AddRef();
    int64_t Release();

    DasResult        Size(size_t* p_out_size);
    DasResult        At(size_t index, IDasPluginInfo** pp_out_info);
    DasRetUInt       Size();
    DasRetPluginInfo At(size_t index);

    void AddInfo(std::unique_ptr<DasPluginInfoImpl>&& up_plugin_info);

    operator IDasPluginInfoVectorImpl*() noexcept;
    operator IDasSwigPluginInfoVectorImpl*() noexcept;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_IDASPLUGINMANAGERIMPL_H
