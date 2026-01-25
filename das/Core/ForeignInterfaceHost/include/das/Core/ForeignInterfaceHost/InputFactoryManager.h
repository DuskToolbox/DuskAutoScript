#ifndef DAS_CORE_FOREIGNINTERFACEHOST_INPUTFACTORYMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_INPUTFACTORYMANAGER_H

#include "das/_autogen/idl/wrapper/Das.ExportInterface.IDasInputFactoryVector.hpp"
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class InputFactoryManager
{
private:
    std::vector<PluginInterface::DasInputFactory> common_input_factory_vector_;

public:
    DasResult Register(Das::PluginInterface::IDasInputFactory* p_factory);

    DasResult FindInterface(
        const DasGuid&                           iid,
        Das::PluginInterface::IDasInputFactory** pp_out_factory);

    // 反正都是在自己模块中调用，直接内部用at实现，外面记得接异常
    void At(
        size_t                                          index,
        DasPtr<Das::PluginInterface::IDasInputFactory>& ref_out_factory);

    // 这里也是
    void Find(
        const DasGuid&                           iid,
        Das::PluginInterface::IDasInputFactory** pp_out_factory);

    auto GetVector() const -> decltype(common_input_factory_vector_);
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_INPUTFACTORYMANAGER_H
