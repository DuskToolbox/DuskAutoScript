#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/InputFactoryManager.h>

#include <das/Core/Logger/Logger.h>
#include <das/DasException.hpp>
#include <das/Utils/CommonUtils.hpp>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DasResult ForeignInterfaceHost::InputFactoryManager::Register(
    Das::PluginInterface::IDasInputFactory* p_factory)
{
    try
    {
        common_input_factory_vector_.emplace_back(p_factory);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult InputFactoryManager::FindInterface(
    const DasGuid&                           iid,
    Das::PluginInterface::IDasInputFactory** pp_out_factory)
{
    DAS_UTILS_CHECK_POINTER(pp_out_factory)
    if (const auto factory_it = std::find_if(
            DAS_FULL_RANGE_OF(common_input_factory_vector_),
            [&iid](const auto& p_factory)
            {
                try
                {
                    const auto factory_iid = p_factory.GetGuid();
                    return factory_iid == iid;
                }
                catch (const DasException& ex)
                {
                    DAS_CORE_LOG_EXCEPTION(ex);
                    return false;
                }
            });
        factory_it != common_input_factory_vector_.end())
    {
        const auto p_result = factory_it->Get();
        *pp_out_factory = p_result;
        p_result->AddRef();
        return DAS_S_OK;
    }
    return DAS_E_NO_INTERFACE;
}

void InputFactoryManager::At(
    size_t                                          index,
    DasPtr<Das::PluginInterface::IDasInputFactory>& ref_out_factory)
{
    ref_out_factory = common_input_factory_vector_.at(index).GetPtr();
}

void InputFactoryManager::Find(
    const DasGuid&                           iid,
    Das::PluginInterface::IDasInputFactory** pp_out_factory)
{
    if (pp_out_factory == nullptr)
    {
        DAS_THROW_EC(DAS_E_INVALID_POINTER);
    }

    const auto result = std::ranges::find_if(
        common_input_factory_vector_,
        [iid](const auto& item)
        {
            DasGuid    guid;
            const auto gg_result = item->GetGuid(&guid);
            if (DAS::IsFailed(gg_result))
            {
                return false;
            }
            return guid == iid;
        });

    if (result == common_input_factory_vector_.end())
    {
        DAS_THROW_EC(DAS_E_OUT_OF_RANGE);
    }

    const auto p_result = result->Get();
    *pp_out_factory = p_result;
    p_result->AddRef();
}

auto InputFactoryManager::GetVector() const
    -> decltype(common_input_factory_vector_)
{
    return common_input_factory_vector_;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
