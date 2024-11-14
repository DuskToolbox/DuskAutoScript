#ifndef DAS_CORE_FOREIGNINTERFACEHOST_INPUTFACTORYMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_INPUTFACTORYMANAGER_H

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/PluginInterface/IDasInput.h>
#include <utility>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class InputFactoryManager
{
public:
    using Type =
        std::pair<DasPtr<IDasInputFactory>, DasPtr<IDasSwigInputFactory>>;

private:
    std::vector<Type> common_input_factory_vector_;

public:
    DasResult Register(IDasInputFactory* p_factory);
    DasResult Register(IDasSwigInputFactory* p_factory);

    DasResult FindInterface(const DasGuid& iid, IDasInputFactory** pp_out_factory);

    // 反正都是在自己模块中调用，直接内部用at实现，外面记得接异常
    void At(size_t index, DasPtr<IDasInputFactory>& ref_out_factory);
    void At(size_t index, DasPtr<IDasSwigInputFactory>& ref_out_factory);
    // 这里也是
    void Find(const DasGuid& iid, IDasInputFactory** pp_out_factory);
    void Find(const DasGuid& iid, IDasSwigInputFactory** pp_out_swig_factory);

    auto GetVector() const -> decltype(common_input_factory_vector_);
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_INPUTFACTORYMANAGER_H
