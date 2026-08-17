#include <das/Core/IPC/CurrentIpcContextScope.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace
        {
            // 单一编译单元持有（随 libDasCore 导出）：此前 inline
            // thread_local 使每个 DLL 各持一份副本，Host BusinessThread
            // 在 libDasCore 设置的上下文对其它 DLL（插件）不可见，跨 DLL
            // 读取恒为空。插件侧 resolve 主进程服务必须走 C ABI
            // （DasQueryMainProcessInterfaceByName），不得经此内部接口。
            thread_local IResolveContext* g_current_context = nullptr;
        } // namespace

        IResolveContext* GetCurrentIpcContext() { return g_current_context; }

        void SetCurrentIpcContext(IResolveContext* context)
        {
            g_current_context = context;
        }

    } // namespace IPC
} // namespace Core
DAS_NS_END
