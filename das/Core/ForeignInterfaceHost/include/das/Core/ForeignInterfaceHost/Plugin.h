#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGIN_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGIN_H

#include "ForeignInterfaceHost.h"
#include "IDasPluginManagerImpl.h"
#include <das/DasPtr.hpp>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHostEnum.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/IDasBase.h>
#include <unordered_set>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

/**
 * @brief 尚不支持热重载
 *
 */
class Plugin
{
    friend class PluginManager;

    DasPtr<IForeignLanguageRuntime> p_runtime_{};
    CommonPluginPtr                 p_plugin_{};
    std::shared_ptr<PluginDesc>     sp_desc_{};
    DasResult                       load_state_{
        DAS_E_UNDEFINED_RETURN_VALUE}; // NOTE: 4 byte padding here.
    DasPtr<IDasReadOnlyString> load_error_message_{};

public:
    Plugin(
        DasPtr<IForeignLanguageRuntime> p_runtime,
        CommonPluginPtr                 p_plugin,
        std::unique_ptr<PluginDesc>     up_desc);
    /**
     * @brief 出错时使用此构造函数
     *
     * @param load_state
     * @param p_error_message
     */
    Plugin(DasResult load_state, IDasReadOnlyString* p_error_message);
    Plugin(Plugin&& other) noexcept;

    explicit operator bool() const noexcept;

    auto GetInfo() const -> std::unique_ptr<DasPluginInfoImpl>;

    ~Plugin();
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGIN_H
