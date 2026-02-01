#ifndef DAS_PLUGINS_DASADBTOUCH_PLUGINIMPL_H
#define DAS_PLUGINS_DASADBTOUCH_PLUGINIMPL_H

#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasPluginPackage.Implements.hpp>

DAS_NS_BEGIN

class DasAdbTouchPlugin final
    : public PluginInterface::DasPluginPackageImplBase<DasAdbTouchPlugin>
{

    DasPtr<DAS::PluginInterface::IDasBasicErrorLens> g_error_lens;

public:
    DAS_UTILS_IDASBASE_AUTO_IMPL(DasAdbTouchPlugin)
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_object) override;
    // IDasPluginPackage
    DAS_IMPL EnumFeature(
        size_t                             index,
        PluginInterface::DasPluginFeature* p_out_feature) override;
    DAS_IMPL CreateFeatureInterface(uint64_t index, void** pp_out_interface)
        override;
    DAS_IMPL CanUnloadNow(bool* p_can_unload) override;
};

DAS_NS_END

#endif // DAS_PLUGINS_DASADBTOUCH_PLUGINIMPL_H
