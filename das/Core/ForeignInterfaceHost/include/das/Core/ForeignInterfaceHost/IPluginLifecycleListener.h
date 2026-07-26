#ifndef DAS_CORE_FOREIGN_INTERFACE_HOST_I_PLUGIN_LIFECYCLE_LISTENER_H
#define DAS_CORE_FOREIGN_INTERFACE_HOST_I_PLUGIN_LIFECYCLE_LISTENER_H

#include <cstdint>
#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

/**
 * @brief 插件生命周期阶段
 *
 * 仅区分 Loaded / Unloading 两阶段 —— D-87-08 的全清 cache 语义不依赖
 * 更细粒度（任何 plugin 变动都触发
 * SchedulerService::InvalidateAuthoringSessions 全清重建）。
 */
enum class PluginLifecyclePhase : uint8_t
{
    Loaded,
    Unloading,
};

/**
 * @brief 外部插件生命周期订阅接口
 *
 * Core 内部订阅 PluginManager 的 load/unload 事件（当前唯一订阅者是
 * SchedulerService，用于在 plugin 变动后失效 authoring cache，落实 D-87-08）。
 *
 * 与既有 3 个内部 sub-manager（ComponentFactoryManager /
 * ErrorLensManager / TaskComponentFactoryManager 的 OnPluginLoaded /
 * OnPluginUnloading）风格对称，区别在于本接口面向 Core 内部外部订阅者，
 * 是一个纯虚类（不继承 IDasBase，不跨 ABI 边界 —— 订阅者均为 Core 内部
 * service，寿命由 PluginManager 的协作者关系保证）。
 *
 * 调用契约：
 * - PluginManager 在释放 mutex_ 后才触发 OnPluginLifecycleEvent
 *   （参考 Shutdown 两阶段 collect -> release -> callback 范式），避免
 *   PluginManager::mutex_ 与订阅者内部锁（如 SchedulerService::mutex_）
 *   形成 AB-BA。订阅者实现不得重新获取 PluginManager::mutex_。
 * - plugin_guid 按值传递，订阅者可安全持有副本；订阅者不应保留指向
 *   PluginManager 内部的引用。
 * - 单订阅者模型：SetPluginLifecycleListener 后注册覆盖前一次注册。
 */
class IPluginLifecycleListener
{
public:
    virtual ~IPluginLifecycleListener() = default;

    /**
     * @brief 插件生命周期事件回调
     * @param phase 生命周期阶段（Loaded / Unloading）
     * @param plugin_guid 发生变动的插件 GUID（按值传递）
     *
     * 实现不应抛异常（noexcept-friendly 调用上下文）；不应重新获取
     * PluginManager::mutex_。
     */
    virtual void OnPluginLifecycleEvent(
        PluginLifecyclePhase phase,
        const DasGuid&       plugin_guid) = 0;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGN_INTERFACE_HOST_I_PLUGIN_LIFECYCLE_LISTENER_H
