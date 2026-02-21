#ifndef DAS_CORE_FOREIGNINTERFACEHOST_IPLUGINMANAGER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_IPLUGINMANAGER_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/DasExport.h>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

/**
 * @brief 插件管理器接口
 *
 * 负责接收插件加载完成的通知，让主进程的 PluginManager
 * 考虑是否持有返回的 IDasBase（即 Proxy）。
 */
struct IDasPluginManager
{
    virtual ~IDasPluginManager() = default;

    /**
     * @brief 插件加载完成回调
     *
     * @param result 插件加载操作结果
     * @param proxy 返回的插件代理对象（IDasBase 接口指针）
     * @return DasResult 操作结果
     */
    virtual DasResult DAS_STD_CALL
    OnPluginLoaded(DasResult result, IDasBase* proxy) = 0;
};

/**
 * @brief 获取插件管理器实例
 *
 * 工厂函数，用于获取 IDasPluginManager 的单例实例。
 *
 * @return IDasPluginManager& 插件管理器引用
 */
DAS_API IDasPluginManager& GetPluginManager();

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_IPLUGINMANAGER_H
