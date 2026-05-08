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
    : inner_(std::move(inner)),
      factory_name_(SafeName(p_factory_name))
{
}

DasResult DAS_STD_CALL
DebugInputFactoryDecorator::GetGuid(DasGuid* p_out_guid)
{
    if (!inner_)
    {
        return DAS_E_INVALID_POINTER;
    }
    return inner_->GetGuid(p_out_guid);
}

DasResult DAS_STD_CALL
DebugInputFactoryDecorator::GetRuntimeClassName(
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

    Das::PluginInterface::IDasInput* p_raw_input = nullptr;
    const auto result = inner_->CreateInstance(p_json_config, &p_raw_input);
    if (result < 0)
    {
        if (p_raw_input)
        {
            p_raw_input->Release();
        }
        return result;
    }

    if (!p_raw_input)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto* p_decorated_input =
        MaybeDecorateInput(p_raw_input, factory_name_.c_str());
    if (!p_decorated_input)
    {
        p_raw_input->Release();
        return DAS_E_INVALID_POINTER;
    }

    *pp_out_input = p_decorated_input;
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

    return Das::DasPtr<Das::PluginInterface::IDasInputFactory>::Attach(
        DebugInputFactoryDecorator::MakeRaw(
            std::move(factory),
            p_factory_name));
}

DAS_CORE_DEBUG_NS_END
