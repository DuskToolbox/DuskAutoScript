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
    Das::PluginInterface::IDasInputFactory* p_inner,
    const char*                             p_factory_name)
    : inner_(Das::DasPtr<Das::PluginInterface::IDasInputFactory>::Attach(
          p_inner)),
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
    *pp_out_input = nullptr;

    if (!inner_)
    {
        return DAS_E_INVALID_POINTER;
    }

    Das::PluginInterface::IDasInput* p_raw_input = nullptr;
    const auto result = inner_->CreateInstance(p_json_config, &p_raw_input);
    if (result < 0 || !p_raw_input)
    {
        if (p_raw_input)
        {
            p_raw_input->Release();
        }
        return result;
    }

    *pp_out_input =
        MaybeDecorateInput(p_raw_input, factory_name_.c_str());
    return result;
}

Das::PluginInterface::IDasInputFactory* MaybeDecorateInputFactory(
    Das::PluginInterface::IDasInputFactory* p_raw,
    const char*                             p_factory_name)
{
    if (!p_raw || !DebugRuntime::IsEnabled())
    {
        return p_raw;
    }

    return DebugInputFactoryDecorator::MakeRaw(p_raw, p_factory_name);
}

DAS_CORE_DEBUG_NS_END
