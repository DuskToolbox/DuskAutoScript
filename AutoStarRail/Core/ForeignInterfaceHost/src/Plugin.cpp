#include <AutoStarRail/Core/ForeignInterfaceHost/Plugin.h>

#include <utility>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

Plugin::Plugin(
    AsrPtr<IForeignLanguageRuntime> p_runtime,
    CommonPluginPtr                 p_plugin,
    std::unique_ptr<PluginDesc>     up_desc)
    : p_runtime_{std::move(p_runtime)}, p_plugin_{std::move(p_plugin)},
      sp_desc_{std::move(up_desc)}, load_state_{ASR_S_OK}, load_error_message_{}
{
}

Plugin::Plugin(AsrResult load_state, IAsrReadOnlyString* p_error_message)
    : load_state_{load_state}, load_error_message_{p_error_message}
{
}

Plugin::~Plugin() = default;

Plugin::operator bool() const noexcept { return IsOk(load_state_); }

ASR_NS_ANONYMOUS_DETAILS_BEGIN

ASR_NS_ANONYMOUS_DETAILS_END

auto Plugin::GetInfo() const -> std::unique_ptr<AsrPluginInfoImpl>
{
    return std::make_unique<AsrPluginInfoImpl>(sp_desc_);
}

Plugin::Plugin(Plugin&& other) noexcept
    : p_runtime_{std::move(other.p_runtime_)},
      p_plugin_{std::move(other.p_plugin_)},
      sp_desc_{std::move(other.sp_desc_)},
      load_state_{
          std::exchange(other.load_state_, ASR_E_UNDEFINED_RETURN_VALUE)},
      load_error_message_{std::move(other.load_error_message_)}
{
}

ASR_CORE_FOREIGNINTERFACEHOST_NS_END
