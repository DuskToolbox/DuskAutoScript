#ifndef DAS_PLUGINS_IPCTESTPLUGIN2_ECHOCOMPONENTSHAREDSTATE_H
#define DAS_PLUGINS_IPCTESTPLUGIN2_ECHOCOMPONENTSHAREDSTATE_H

#include <cstdint>

#include <das/DasConfig.h>
#include <das/DasExport.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_INTERPROCESS_WARNING
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
DAS_DISABLE_WARNING_END

DAS_NS_BEGIN

// 测试 exe 与 IpcTestPlugin2 dll 共享的析构观测状态（布局单一来源）：
// exe 创建 managed_shared_memory 段并把本结构构造在其中，段名经
// kIpcTestPlugin2ShmEnvName 环境变量传给 host 子进程内的插件（open_only
// 接入）。EchoTaskComponent 构造/析构在此写计数并 notify——跨 host 释放
// 链（RELEASE 中转 → owner 记账 → 条目回收）的对象析构由此可被测试进程
// 观测。环境变量缺失时插件解析结果为空指针，信号路径静默跳过（零行为
// 影响）。段名由 exe 侧以 pid + 序号唯一化，生命周期随测试。
inline constexpr char kIpcTestPlugin2ShmEnvName[] =
    "DAS_IPC_TEST_PLUGIN2_SHM_NAME";

using EchoIpcMutex = boost::interprocess::interprocess_mutex;
using EchoIpcCondition = boost::interprocess::interprocess_condition;
using EchoIpcScopedLock = boost::interprocess::scoped_lock<EchoIpcMutex>;

struct EchoComponentSharedState
{
    EchoIpcMutex     mutex;
    EchoIpcCondition cv;

    int32_t created_count = 0;
    int32_t destroyed_count = 0;
};

DAS_NS_END

#endif // DAS_PLUGINS_IPCTESTPLUGIN2_ECHOCOMPONENTSHAREDSTATE_H
