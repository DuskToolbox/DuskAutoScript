#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINFILEMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINFILEMANAGER_H

#include "Plugin.h"
#include <das/Core/ForeignInterfaceHost/ComponentFactoryManager.h>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/ForeignInterfaceHost/ErrorLensManager.h>
#include <das/Core/ForeignInterfaceHost/InputFactoryManager.h>
#include <das/Core/ForeignInterfaceHost/TaskManager.h>
#include <das/PluginInterface/IDasCapture.h>
#include <das/Utils/Expected.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

template <class T, class SwigT>
class InterfaceManager
{
private:
    struct PluginInterface
    {
        DasPtr<T>     cpp_interface;
        DasPtr<SwigT> swig_interface;
    };
    using GuidInterfaceMap = std::map<DasGuid, PluginInterface>;

    GuidInterfaceMap map_;

    void InternalAddInterface(
        const PluginInterface& plugin_interface,
        const DasGuid&         plugin_guid)
    {
        if (auto plugin_item = map_.find(plugin_guid);
            plugin_item != map_.end())
        {
            DAS_CORE_LOG_WARN(
                "Duplicate interface registration for plugin guid: {}.",
                plugin_guid);
        }
        map_[plugin_guid] = plugin_interface;
    }

public:
    InterfaceManager() = default;

    DasResult Register(DasPtr<T> p_interface, const DasGuid& interface_guid)
    {
        PluginInterface plugin_interface;

        try
        {
            plugin_interface.swig_interface =
                MakeDasPtr<CppToSwig<T>>(p_interface);
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }

        plugin_interface.cpp_interface = std::move(p_interface);

        InternalAddInterface(plugin_interface, interface_guid);

        return DAS_S_OK;
    }

    DasResult Register(DasPtr<SwigT> p_interface, const DasGuid& interface_guid)
    {
        PluginInterface  plugin_interface;
        SwigToCpp<SwigT> p_cpp_interface;

        try
        {
            plugin_interface.cpp_interface =
                MakeDasPtr<SwigToCpp<SwigT>>(p_interface);
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }

        plugin_interface.swig_interface = std::move(p_interface);

        InternalAddInterface(plugin_interface, interface_guid);

        return DAS_S_OK;
    }
};

class PluginManager;

class IDasPluginManagerForUiImpl final : public IDasPluginManagerForUi
{
    PluginManager& impl_;

public:
    IDasPluginManagerForUiImpl(PluginManager& impl);
    // IDasBase
    int64_t   AddRef() override;
    int64_t   Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;
    // IDasPluginManagerForUi
    DasResult GetAllPluginInfo(
        IDasPluginInfoVector** pp_out_plugin_info_vector) override;
    DasResult FindInterface(const DasGuid& iid, void** pp_object) override;
    DasResult GetPluginSettingsJson(
        const DasGuid&       plugin_guid,
        IDasReadOnlyString** pp_out_json) override;
    DasResult SetPluginSettingsJson(
        const DasGuid&      plugin_guid,
        IDasReadOnlyString* p_json) override;
    DasResult ResetPluginSettings(const DasGuid& plugin_guid) override;
};

class IDasPluginManagerImpl final : public IDasPluginManager
{
    PluginManager& impl_;

public:
    IDasPluginManagerImpl(PluginManager& impl);
    // IDasBase
    int64_t   AddRef() override;
    int64_t   Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;
    // IDasPluginManager
    DasResult CreateComponent(
        const DasGuid&  iid,
        IDasComponent** pp_out_component) override;
    DasResult CreateCaptureManager(
        IDasReadOnlyString*  p_environment_config,
        IDasCaptureManager** pp_out_capture_manager) override;
};

class IDasSwigPluginManagerImpl final : public IDasSwigPluginManager
{
    PluginManager& impl_;

public:
    IDasSwigPluginManagerImpl(PluginManager& impl);

    // IDasSwigBase
    int64_t        AddRef() override;
    int64_t        Release() override;
    DasRetSwigBase QueryInterface(const DasGuid& iid) override;
    // IDasSwigPluginManager
    DasRetComponent      CreateComponent(const DasGuid& iid) override;
    DasRetCaptureManager CreateCaptureManager(
        DasReadOnlyString environment_config) override;
};

/**
 * @brief 构造函数中不得出现与IDas系列API相关的操作，以保证线程安全。
 *   调用此类的任意公开函数时，都会对线程id进行检查，不符合将返回
 * DAS_E_UNEXPECTED_THREAD_DETECTED
 *
 */
class PluginManager
    : DAS_UTILS_MULTIPLE_PROJECTION_GENERATORS(
          PluginManager,
          IDasPluginManagerImpl,
          IDasSwigPluginManagerImpl),
      public Utils::
          ProjectionGenerator<PluginManager, IDasPluginManagerForUiImpl>,
      public Utils::ThreadVerifier
{
public:
    /**
     * @brief 引用计数对于单例没有意义，因此不使用原子变量
     */
    int64_t ref_counter_{};
    using NamePluginMap =
        std::map<DasPtr<IDasReadOnlyString>, Plugin, DasStringLess>;

    /**
     * @brief READ ONLY struct.
     */
    struct InterfaceStaticStorage
    {
        std::filesystem::path              path;
        std::shared_ptr<PluginPackageDesc> sp_desc;
    };
    using InterfaceStaticStorageMap =
        std::unordered_map<DasGuid, InterfaceStaticStorage>;

private:
    // 调用很乱，锁都不好加
    mutable std::recursive_mutex mutex_{};
    /**
     * @brief 注意：如果连名字都没获取到，则以json路径为此处的名称
     */
    NamePluginMap                           name_plugin_map_{};
    InterfaceStaticStorageMap               guid_storage_map_{};
    TaskManager                             task_manager_{};
    std::vector<DasPtr<IDasCaptureFactory>> capture_factory_vector_{};
    ComponentFactoryManager                 component_factory_manager_{};
    ErrorLensManager                        error_lens_manager_{};
    InputFactoryManager                     input_factory_manager_{};
    bool                                    is_inited_{false};

    IDasPluginManagerImpl cpp_projection_{*this};

    DasResult AddInterface(const Plugin& plugin, const char* u8_plugin_name);
    void      RegisterInterfaceStaticStorage(
             IDasTypeInfo*                 p_interface,
             const InterfaceStaticStorage& storage);
    void RegisterInterfaceStaticStorage(
        IDasSwigTypeInfo*             p_swig_interface,
        const InterfaceStaticStorage& storage);

    static std::unique_ptr<PluginPackageDesc> GetPluginDesc(
        const std::filesystem::path& metadata_path,
        bool                         is_directory);

    DasResult GetInterface(const Plugin& plugin);

public:
    int64_t AddRef();
    int64_t Release();
    /**
     * @brief try to load all plugin. And get all interface.
     * @return DasResult DAS_S_OK when all plugin are loaded successfully.\n
     *         DAS_S_FALSE when some plugin have error.\n
     *         DAS_E_INTERNAL_FATAL_ERROR when any plugin have
     * DAS_E_SWIG_INTERNAL_ERROR or even worse.
     */
    DasResult Refresh(IDasReadOnlyGuidVector* p_ignored_guid_vector);

    /**
     * @brief Get the Error Explanation from DasResult.
     *
     * @param iid guid of plugin
     * @param error_code
     * @param pp_out_error_message
     * @return DasResult
     */
    DasResult GetErrorMessage(
        const DasGuid&       iid,
        DasResult            error_code,
        IDasReadOnlyString** pp_out_error_message);

    bool IsInited() const noexcept;

    DasResult GetAllPluginInfo(
        IDasPluginInfoVector** pp_out_plugin_info_vector);

    auto GetInterfaceStaticStorage(IDasTypeInfo* p_type_info) const
        -> DAS::Utils::Expected<
            std::reference_wrapper<const InterfaceStaticStorage>>;
    auto GetInterfaceStaticStorage(IDasSwigTypeInfo* p_type_info) const
        -> DAS::Utils::Expected<
            std::reference_wrapper<const InterfaceStaticStorage>>;

    DasResult FindInterface(const DasGuid& iid, void** pp_out_object);

    DasResult CreateCaptureManager(
        IDasReadOnlyString*  environment_config,
        IDasCaptureManager** pp_out_manager);
    DasRetCaptureManager CreateCaptureManager(
        DasReadOnlyString environment_config);

    DasResult CreateComponent(
        const DasGuid&  iid,
        IDasComponent** pp_out_component);
    DasRetComponent CreateComponent(const DasGuid& iid);

    DasResult GetPluginSettingsJson(
        const DasGuid&       plugin_guid,
        IDasReadOnlyString** pp_out_json);
    DasResult SetPluginSettingsJson(
        const DasGuid&      plugin_guid,
        IDasReadOnlyString* p_json);
    DasResult ResetPluginSettings(const DasGuid& plugin_guid);
    auto      FindInterfaceStaticStorage(DasGuid iid) -> Utils::Expected<
             std::reference_wrapper<const InterfaceStaticStorage>>;
};

extern PluginManager g_plugin_manager;

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINFILEMANAGER_H
