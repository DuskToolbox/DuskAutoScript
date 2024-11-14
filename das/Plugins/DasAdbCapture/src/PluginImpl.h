#ifndef DAS_PLUGINS_DASADBCAPTURE_PLUGINIMPL_H
#define DAS_PLUGINS_DASADBCAPTURE_PLUGINIMPL_H

#include <das/PluginInterface/IDasPlugin.h>
#include <das/Utils/CommonUtils.hpp>
#include <cstdint>
#include <unordered_map>

// {EAC73FD2-5674-4796-8298-71B80727E993}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das,
    AdbCapturePlugin,
    0xeac73fd2,
    0x5674,
    0x4796,
    0x82,
    0x98,
    0x71,
    0xb8,
    0x7,
    0x27,
    0xe9,
    0x93);

DAS_NS_BEGIN

class AdbCapturePlugin final : public IDasPlugin
{
private:
    DAS::Utils::RefCounter<AdbCapturePlugin> ref_counter_;

public:
    // IDasBase
    int64_t   AddRef() override;
    int64_t   Release() override;
    DasResult QueryInterface(const DasGuid& guid, void** pp_out_object)
        override;
    // IDasPlugin
    DasResult EnumFeature(const size_t index, DasPluginFeature* p_out_feature)
        override;
    DasResult CreateFeatureInterface(size_t index, void** pp_out_interface)
        override;
    DasResult CanUnloadNow() override;
};

void AdbCaptureAddRef();

void AdbCaptureRelease();

DAS_NS_END

#endif // DAS_PLUGINS_DASADBCAPTURE_PLUGINIMPL_H