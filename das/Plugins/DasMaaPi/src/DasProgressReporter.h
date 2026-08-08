#pragma once

#include "PiUpdateService.h"

#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasI18n.h>
#include <das/_autogen/idl/abi/IDasProgressService.h>

#include <atomic>
#include <memory>
#include <string>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    // DasMaaPi 侧推送链路核心。持有 SSE 路由 key + 主进程
    // IDasProgressService proxy；Start() 注册本地 IDasProgressCallback（经
    // SerializeInInterfaceParam 自动代理化），Report() 把 UpdateProgress 装配成
    // IDasProgressUpdate 字段增量上报（复用 setValue payload 字段语义，
    // 无 percent/status/stage——显示由 document 字段值决定），Stop()
    // RemoveCallback 释放 proxy（成对生命周期）。
    // reporter 生命周期被跨进程 callback 反向引用共享（callback proxy 由
    // DasHttp 保活、可能活得比 reporter 久），故 reporter 由 shared_ptr 管理、
    // callback 持 weak_ptr（lock() 判空后访问，杜绝 owner 悬垂）。Start() 用
    // shared_from_this()，要求创建方必须 make_shared。
    class DasProgressReporter final
        : public std::enable_shared_from_this<DasProgressReporter>
    {
    public:
        DasProgressReporter(
            std::string                                       key,
            DasPtr<Das::ExportInterface::IDasProgressService> service);

        // 注册 callback（AddCallback）。失败返回 false（service 不可用）。
        bool Start();
        // 反注册 callback（RemoveCallback）+ 释放 proxy。幂等。
        void Stop();
        // RAII：析构自动 Stop()，异常路径（ApplyUpdate 抛 filesystem 错误）
        // 也保证 RemoveCallback 执行，杜绝 callback 泄漏与 owner 悬垂。
        ~DasProgressReporter();
        // 上报字段增量；未 Start / 已 Stop / 订阅全断开时为空操作。
        void Report(const UpdateProgress& progress);

        // 该 key 所有 SSE 订阅断开（OnAllSubscribersDisconnected 命中 own
        // key）。
        bool IsDisconnected() const { return disconnected_.load(); }

        // 注入外部翻译表（测试缝）。覆盖懒加载：传入非空 i18n 后
        // GetI18n 直接返回它，不再走 this_line_location/i18n 装载。
        // 传空清除注入并重置尝试标志，下次 GetI18n 重新懒加载。
        void SetI18n(DasPtr<Das::ExportInterface::IDasI18n> i18n)
        {
            i18n_ = std::move(i18n);
            i18n_load_attempted_ = (i18n_ != nullptr);
        }

    private:
        class DasProgressCallbackImpl;
        void OnSubscribersDisconnected();

        // 取翻译表：优先注入（SetI18n），否则首次调用懒加载
        // <plugin_dir>/i18n（boost::dll::this_line_location 推导）。
        // 目录缺失/装载失败永久返回 nullptr（不重试），降级硬编码文本。
        Das::ExportInterface::IDasI18n* GetI18n();
        // 把 UpdateProgress 投影成显示文本。i18n 就绪时走
        // Translate(key)+vformat 填 {N}；否则回退硬编码字面量。
        std::string BuildProgressText(const UpdateProgress& progress);

        std::string                                        key_;
        DasPtr<Das::ExportInterface::IDasProgressService>  service_;
        DasPtr<Das::ExportInterface::IDasProgressCallback> callback_;
        DasPtr<Das::ExportInterface::IDasI18n>             i18n_;
        std::atomic<bool>                                  disconnected_{false};
        bool                                               started_ = false;
        bool i18n_load_attempted_ = false;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END
