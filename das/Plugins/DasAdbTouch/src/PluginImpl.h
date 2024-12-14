#ifndef DAS_PLUGINS_DASADBTOUCH_PLUGINIMPL_H
#define DAS_PLUGINS_DASADBTOUCH_PLUGINIMPL_H

#include <das/ExportInterface/IDasBasicErrorLens.h>
#include <das/PluginInterface/IDasPluginPackage.h>
#include <das/Utils/CommonUtils.hpp>

DAS_NS_BEGIN

class DasAdbTouchPlugin final : public IDasPluginPackage
{

    DasPtr<IDasBasicErrorLens> g_error_lens;

public:
    DAS_UTILS_IDASBASE_AUTO_IMPL(DasAdbTouchPlugin)
    DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_object) override;
    // IDasPluginPackage
    DAS_IMPL EnumFeature(size_t index, DasPluginFeature* p_out_feature)
        override;
    DAS_IMPL CreateFeatureInterface(
        size_t           index,
        void**           pp_out_interface) override;
    DAS_IMPL CanUnloadNow() override;
};

DAS_NS_END

#endif // DAS_PLUGINS_DASADBTOUCH_PLUGINIMPL_H
