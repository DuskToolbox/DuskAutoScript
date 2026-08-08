#include "DebugInputFactoryDecorator.h"

#include <das/Core/Debug/DebugDecorators.h>
#include <das/Core/Debug/DebugRuntime.h>

#include <utility>

DAS_CORE_DEBUG_NS_BEGIN
namespace
{
    auto SafeName(const char* p_name) -> std::string
    {
        return p_name && *p_name ? std::string{p_name}
                                 : std::string{"input_factory"};
    }

} // namespace

DebugInputFactoryDecorator::DebugInputFactoryDecorator(
    Das::DasPtr<Das::PluginInterface::IDasInputFactory> inner,
    const char*                                         p_factory_name)
    : inner_(std::move(inner)), factory_name_(SafeName(p_factory_name))
{
}

DasResult DAS_STD_CALL DebugInputFactoryDecorator::GetGuid(DasGuid* p_out_guid)
{
    if (!inner_)
    {
        return DAS_E_INVALID_POINTER;
    }
    return inner_->GetGuid(p_out_guid);
}

DasResult DAS_STD_CALL DebugInputFactoryDecorator::GetRuntimeClassName(
    IDasReadOnlyString** pp_out_name)
{
    if (!inner_)
    {
        return DAS_E_INVALID_POINTER;
    }
    return inner_->GetRuntimeClassName(pp_out_name);
}

DasResult DAS_STD_CALL DebugInputFactoryDecorator::CreateInstance(
    IDasReadOnlyString*               p_json_config,
    Das::PluginInterface::IDasInput** pp_out_input)
{
    if (!pp_out_input)
    {
        return DAS_E_INVALID_POINTER;
    }
    Das::DasOutPtr<Das::PluginInterface::IDasInput> out_input(pp_out_input);

    if (!inner_)
    {
        return DAS_E_INVALID_POINTER;
    }

    Das::DasPtr<Das::PluginInterface::IDasInput> raw_input;
    const auto result = inner_->CreateInstance(p_json_config, raw_input.Put());
    if (result < 0)
    {
        return result;
    }

    if (!raw_input)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto decorated_input =
        MaybeDecorateInput(std::move(raw_input), factory_name_.c_str());
    if (!decorated_input)
    {
        return DAS_E_INVALID_POINTER;
    }

    *pp_out_input = decorated_input.Get();
    (*pp_out_input)->AddRef();
    out_input.Keep();
    return result;
}

Das::DasPtr<Das::PluginInterface::IDasInputFactory> MaybeDecorateInputFactory(
    Das::DasPtr<Das::PluginInterface::IDasInputFactory> factory,
    const char*                                         p_factory_name)
{
    if (!factory || !DebugRuntime::IsEnabled())
    {
        return factory;
    }

    return DebugInputFactoryDecorator::Make(std::move(factory), p_factory_name);
}

DAS_CORE_DEBUG_NS_END
