#ifndef DAS_CORE_FOREIGNINTERFACEHOST_RUNTIMEPROVIDER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_RUNTIMEPROVIDER_H

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHostEnum.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/Expected.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class IRemotePluginHost;

/**
 * @brief 加载请求（ABI 友好）。
 *
 * path 字段为 UTF-8、借用（调用方拥有，LoadPlugin 调用期间有效）。
 * 生命周期回调不在此结构——回调在构造内置 IPC/Node provider 时传入
 * （见 RuntimeLifecycleCallbacks），第三方 provider 不使用回调。
 */
struct RuntimeLoadRequest
{
    const char* manifest_path;
    const char* runtime_path;
    const char* node_modules_root; // 可 nullptr（Node 专用）
    DasGuid     plugin_guid{};
    uint16_t    main_process_owner_session_id = 0;
};

/**
 * @brief 加载结果（ABI 友好）。
 *
 * object 所有权转移给调用方（已 AddRef），调用方负责 Release。
 */
struct RuntimeLoadResult
{
    IDasBase* object = nullptr;
    uint16_t  owner_session_id = 0;
};

/**
 * @brief IPC 生命周期回调。
 *
 * 构造内置 IPC/Node provider 时传入，provider 持有；LoadPlugin 时注册到
 * launcher（host 进程退出 / 心跳超时时触发）。第三方 provider 不使用。
 */
struct RuntimeLifecycleCallbacks
{
    std::function<void(uint16_t session_id, int exit_code)> on_process_exit;
    std::function<void(DasGuid guid)> on_heartbeat_timeout;
};

/**
 * @brief runtime provider：加载策略抽象（进程内 / IPC / Node）。
 *
 * LoadPlugin 参数 ABI 友好（const char* / IDasBase* / DasGuid / uint16_t），
 * 不含 STL。第三方实现本接口注册自定义加载逻辑。
 */
class IRuntimeProvider
{
public:
    virtual ~IRuntimeProvider() = default;

    /**
     * @brief 加载插件。
     * @param request 加载请求（path 借用，调用期间有效）。
     * @param out_result 输出加载结果；成功时 object 为已 AddRef 的 package，
     *                   调用方负责 Release。
     * @return DAS_S_OK 成功，否则错误码。
     */
    virtual DasResult LoadPlugin(
        const RuntimeLoadRequest& request,
        RuntimeLoadResult*        out_result) = 0;
};

struct RuntimeProviderFactoryDesc
{
    ForeignInterfaceLanguage           language{};
    LoadMode                           load_mode{LoadMode::InProcess};
    std::filesystem::path              native_host_exe_path;
    std::unique_ptr<IRemotePluginHost> remote_plugin_host;
};

DAS_API auto CreateLocalRuntimeProvider(
    DAS::DasPtr<IForeignLanguageRuntime> runtime)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>;

DAS_API auto CreateLocalRuntimeProvider(
    const ForeignLanguageRuntimeFactoryDesc& desc)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>;

DAS_API auto CreateNativeIpcRuntimeProvider(
    std::filesystem::path              host_exe_path,
    std::unique_ptr<IRemotePluginHost> remote_plugin_host,
    RuntimeLifecycleCallbacks          callbacks)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>;

DAS_API auto CreateNodeRuntimeProvider(
    std::unique_ptr<IRemotePluginHost> remote_plugin_host,
    RuntimeLifecycleCallbacks          callbacks)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>;

DAS_API auto CreateRuntimeProvider(
    RuntimeProviderFactoryDesc desc,
    RuntimeLifecycleCallbacks  callbacks)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>;

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_RUNTIMEPROVIDER_H
