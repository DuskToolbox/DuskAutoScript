#ifndef DAS_CORE_FOREIGNINTERFACEHOST_IDASPLUGINMANAGERIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_IDASPLUGINMANAGERIMPL_H

#include "ForeignInterfaceHost.h"

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasPluginInfo.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasPluginInfoVector.Implements.hpp>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

using namespace DAS::ExportInterface;

class DasPluginInfoImpl : public DasPluginInfoImplBase<DasPluginInfoImpl>
{
    std::shared_ptr<PluginPackageDesc> sp_desc_;

    template <auto MemberPointer>
    DAS_IMPL GetStringImpl(IDasReadOnlyString** pp_out_string);
    template <auto MemberPointer>
    DasRetReadOnlyString GetDasStringImpl();

public:
    DasPluginInfoImpl(std::shared_ptr<PluginPackageDesc> sp_desc);

    DasResult GetName(IDasReadOnlyString** pp_out_name) override;
    DasResult GetDescription(IDasReadOnlyString** pp_out_description) override;
    DasResult GetAuthor(IDasReadOnlyString** pp_out_author) override;
    DasResult GetVersion(IDasReadOnlyString** pp_out_version) override;
    DasResult GetSupportedSystem(
        IDasReadOnlyString** pp_out_supported_system) override;
    DasResult GetPluginIid(DasGuid* p_out_guid) override;
    DasResult GetPluginSettingsDescriptor(
        IDasReadOnlyString** pp_out_string) override;
};

class DasPluginInfoVectorImpl final
    : public DasPluginInfoVectorImplBase<DasPluginInfoVectorImpl>
{
    DAS::Utils::RefCounter<DasPluginInfoVectorImpl> ref_counter_{};
    std::vector<DasPtr<DasPluginInfoImpl>>          plugin_info_vector_{};

public:
    DasResult Size(size_t* p_out_size) override;
    DasResult At(size_t index, IDasPluginInfo** pp_out_info) override;

    void AddInfo(DasPtr<DasPluginInfoImpl> sp_plugin_info);
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_IDASPLUGINMANAGERIMPL_H
