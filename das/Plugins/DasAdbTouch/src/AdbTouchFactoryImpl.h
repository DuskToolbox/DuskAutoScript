#ifndef DAS_PLUGINS_DASADBTOUCH_ADBTOUCHFACTORYIMPL_H
#define DAS_PLUGINS_DASADBTOUCH_ADBTOUCHFACTORYIMPL_H

#include <das/_autogen/idl/abi/IDasInput.h>
#include <das/Utils/CommonUtils.hpp>

DAS_NS_BEGIN

using namespace Das::PluginInterface;

class AdbTouchFactory final : public IDasInputFactory
{
public:
    // IDasBase
    DAS_UTILS_IDASBASE_AUTO_IMPL(AdbTouchFactory);
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;
    // IDasTypeInfo
    DasResult GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override;
    DasResult GetGuid(DasGuid* p_out_guid) override;
    // IDasInputFactory
    DasResult CreateInstance(
        IDasReadOnlyString* p_json_config,
        IDasInput**         pp_out_input) override;
};

DAS_NS_END

#endif // DAS_PLUGINS_DASADBTOUCH_ADBTOUCHFACTORYIMPL_H
