#include <AutoStarRail/Core/ForeignInterfaceHost/ComponentFactoryManager.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <AutoStarRail/Core/Logger/Logger.h>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

auto ComponentFactoryManager::FindSupportedFactory(const AsrGuid& iid)
    -> decltype(common_component_factory_vector_)::const_iterator
{
    return std::find_if(
        ASR_FULL_RANGE_OF(common_component_factory_vector_),
        [&iid](const auto& factory)
        {
            return std::visit(
                Utils::overload_set{[&iid](const auto& p_factory)
                                    { return p_factory->IsSupported(iid); }},
                factory);
        });
}

AsrResult ComponentFactoryManager::Register(IAsrComponentFactory* p_factory)
{
    try
    {
        common_component_factory_vector_.emplace_back(p_factory);
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
    return ASR_S_OK;
}

AsrResult ComponentFactoryManager::Register(IAsrSwigComponentFactory* p_factory)
{
    try
    {
        common_component_factory_vector_.emplace_back(p_factory);
    }
    catch (const std::bad_alloc&)
    {
        return ASR_E_OUT_OF_MEMORY;
    }
    return ASR_S_OK;
}

AsrResult ComponentFactoryManager::CreateObject(
    const AsrGuid&  iid,
    IAsrComponent** pp_out_component)
{
    const auto p_factory = FindSupportedFactory(iid);
    if (p_factory == common_component_factory_vector_.end())
    {
        return ASR_E_NO_INTERFACE;
    }

    return std::visit(
        Utils::overload_set{
            [&iid, pp_out_component](
                const AsrPtr<IAsrComponentFactory>& p_cpp_factory)
            { return p_cpp_factory->CreateInstance(iid, pp_out_component); },
            [&iid, pp_out_component](
                const AsrPtr<IAsrSwigComponentFactory>& p_swig_factory)
            {
                const auto ret_result = p_swig_factory->CreateInstance(iid);
                if (IsFailed(ret_result))
                {
                    return ret_result.error_code;
                }

                auto cpp_result = MakeInterop<IAsrComponent>(ret_result.value);
                if (!cpp_result)
                {
                    return cpp_result.error();
                }

                auto& p_out_component = *pp_out_component;
                p_out_component = cpp_result.value().Get();
                p_out_component->AddRef();
                return ret_result.error_code;
            }},
        *p_factory);
}

AsrRetComponent ComponentFactoryManager::CreateObject(const AsrGuid& iid)
{
    const auto p_factory = FindSupportedFactory(iid);
    if (p_factory == common_component_factory_vector_.end())
    {
        return {ASR_E_NO_INTERFACE};
    }

    AsrRetComponent result{};

    std::visit(
        Utils::overload_set{
            [&iid, &result](const AsrPtr<IAsrComponentFactory>& p_cpp_factory)
            {
                AsrPtr<IAsrComponent> cpp_result;
                result.error_code =
                    p_cpp_factory->CreateInstance(iid, cpp_result.Put());
                if (IsFailed(result))
                {
                    return;
                }

                auto expected_swig_result =
                    MakeInterop<IAsrSwigComponent>(cpp_result);
                if (!expected_swig_result)
                {
                    ASR_CORE_LOG_WARN(
                        "Call CreateInstance return {}.",
                        result.error_code);
                    result = {expected_swig_result.error()};
                }

                result.value = expected_swig_result.value();
            },
            [&iid,
             &result](const AsrPtr<IAsrSwigComponentFactory>& p_swig_factory)
            { result = p_swig_factory->CreateInstance(iid); }},
        *p_factory);

    return result;
}

ASR_CORE_FOREIGNINTERFACEHOST_NS_END
