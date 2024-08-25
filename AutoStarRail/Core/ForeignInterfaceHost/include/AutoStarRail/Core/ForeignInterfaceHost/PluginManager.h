#ifndef ASR_CORE_FOREIGNINTERFACEHOST_PLUGINFILEMANAGER_H
#define ASR_CORE_FOREIGNINTERFACEHOST_PLUGINFILEMANAGER_H

#include "Plugin.h"
#include <AutoStarRail/Core/ForeignInterfaceHost/AsrGuid.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/AsrStringImpl.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/ComponentFactoryManager.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/Config.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/ErrorLensManager.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/InputFactoryManager.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/TaskManager.h>
#include <AutoStarRail/PluginInterface/IAsrCapture.h>
#include <AutoStarRail/Utils/Expected.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <vector>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

template <class T, class SwigT>
class InterfaceManager
{
private:
    struct PluginInterface
    {
        AsrPtr<T>     cpp_interface;
        AsrPtr<SwigT> swig_interface;
    };
    using GuidInterfaceMap = std::map<AsrGuid, PluginInterface>;

    GuidInterfaceMap map_;

    void InternalAddInterface(
        const PluginInterface& plugin_interface,
        const AsrGuid&         plugin_guid)
    {
        if (auto plugin_item = map_.find(plugin_guid);
            plugin_item != map_.end())
        {
            ASR_CORE_LOG_WARN(
                "Duplicate interface registration for plugin guid: {}.",
                plugin_guid);
        }
        map_[plugin_guid] = plugin_interface;
    }

public:
    InterfaceManager() = default;

    AsrResult Register(AsrPtr<T> p_interface, const AsrGuid& interface_guid)
    {
        PluginInterface plugin_interface;

        try
        {
            plugin_interface.swig_interface =
                MakeAsrPtr<CppToSwig<T>>(p_interface);
        }
        catch (const std::bad_alloc&)
        {
            return ASR_E_OUT_OF_MEMORY;
        }

        plugin_interface.cpp_interface = std::move(p_interface);

        InternalAddInterface(plugin_interface, interface_guid);

        return ASR_S_OK;
    }

    AsrResult Register(AsrPtr<SwigT> p_interface, const AsrGuid& interface_guid)
    {
        PluginInterface  plugin_interface;
        SwigToCpp<SwigT> p_cpp_interface;

        try
        {
            plugin_interface.cpp_interface =
                MakeAsrPtr<SwigToCpp<SwigT>>(p_interface);
        }
        catch (const std::bad_alloc&)
        {
            return ASR_E_OUT_OF_MEMORY;
        }

        plugin_interface.swig_interface = std::move(p_interface);

        InternalAddInterface(plugin_interface, interface_guid);

        return ASR_S_OK;
    }
};

class PluginManager;

class IAsrPluginManagerImpl : public IAsrPluginManager
{
    PluginManager& impl_;

public:
    IAsrPluginManagerImpl(PluginManager& impl);
    // IAsrBase
    int64_t   AddRef() override;
    int64_t   Release() override;
    AsrResult QueryInterface(const AsrGuid& iid, void** pp_object) override;
    // IAsrPluginManager
    AsrResult GetAllPluginInfo(
        IAsrPluginInfoVector** pp_out_plugin_info_vector) override;
    AsrResult FindInterface(const AsrGuid& iid, void** pp_object) override;
};

class PluginManager
{
public:
    /**
     * @brief 引用计数对于单例没有意义，因此不使用原子变量
     */
    int64_t ref_counter_{};
    using NamePluginMap =
        std::map<AsrPtr<IAsrReadOnlyString>, Plugin, AsrStringLess>;

    /**
     * @brief READ ONLY struct.
     */
    struct InterfaceStaticStorage
    {
        std::filesystem::path       path;
        std::shared_ptr<PluginDesc> sp_desc;
    };
    using InterfaceStaticStorageMap =
        std::unordered_map<AsrGuid, InterfaceStaticStorage>;

private:
    /**
     * @brief 注意：如果连名字都没获取到，则以json路径为此处的名称
     */
    NamePluginMap                           name_plugin_map_{};
    InterfaceStaticStorageMap               guid_storage_map_{};
    TaskManager                             task_manager_{};
    std::vector<AsrPtr<IAsrCaptureFactory>> capture_factory_vector_{};
    ComponentFactoryManager                 component_factory_manager_{};
    ErrorLensManager                        error_lens_manager_{};
    InputFactoryManager                     input_factory_manager_{};

    IAsrPluginManagerImpl cpp_projection_{*this};

    AsrResult AddInterface(const Plugin& plugin, const char* u8_plugin_name);
    void      RegisterInterfaceStaticStorage(
             IAsrTypeInfo*                 p_interface,
             const InterfaceStaticStorage& storage);
    void RegisterInterfaceStaticStorage(
        IAsrSwigTypeInfo*             p_swig_interface,
        const InterfaceStaticStorage& storage);

    static std::unique_ptr<PluginDesc> GetPluginDesc(
        const std::filesystem::path& metadata_path,
        bool                         is_directory);

    AsrResult GetInterface(const Plugin& plugin);

public:
    int64_t AddRef();
    int64_t Release();
    /**
     * @brief try to load all plugin. And get all interface.
     * @return AsrResult ASR_S_OK when all plugin are loaded successfully.\n
     *         ASR_S_FALSE when some plugin have error.\n
     *         ASR_E_INTERNAL_FATAL_ERROR when any plugin have
     * ASR_E_SWIG_INTERNAL_ERROR or even worse.
     */
    AsrResult Refresh(IAsrReadOnlyGuidVector* p_ignored_guid_vector);

    /**
     * @brief Get the Error Explanation from AsrResult.
     *
     * @param iid guid of plugin
     * @param error_code
     * @param pp_out_error_message
     * @return AsrResult
     */
    AsrResult GetErrorMessage(
        const AsrGuid&       iid,
        AsrResult            error_code,
        IAsrReadOnlyString** pp_out_error_message);

    AsrResult GetAllPluginInfo(
        IAsrPluginInfoVector** pp_out_plugin_info_vector);

    auto GetInterfaceStaticStorage(IAsrTypeInfo* p_type_info) const
        -> ASR::Utils::Expected<
            std::reference_wrapper<const InterfaceStaticStorage>>;
    auto GetInterfaceStaticStorage(IAsrSwigTypeInfo* p_type_info) const
        -> ASR::Utils::Expected<
            std::reference_wrapper<const InterfaceStaticStorage>>;

    auto GetAllCaptureFactory() const noexcept
        -> const std::vector<AsrPtr<IAsrCaptureFactory>>&;

    AsrResult FindInterface(const AsrGuid& iid, void** pp_out_object);

    operator IAsrPluginManagerImpl*() noexcept;
};

extern PluginManager g_plugin_manager;

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // ASR_CORE_FOREIGNINTERFACEHOST_PLUGINFILEMANAGER_H
