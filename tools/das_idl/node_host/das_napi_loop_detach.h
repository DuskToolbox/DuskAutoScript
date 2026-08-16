// DAS Node addon 事件循环引用注册表。
// 由 das_napi_export.py 分发到生成目录（与 das-node-host.cjs 同路径），
// 生成 cpp 以引号 include 引用，勿在此处引入 napi.h（Release 细节归子类）。
#ifndef DAS_NAPI_LOOP_DETACH_H
#define DAS_NAPI_LOOP_DETACH_H

#include <mutex>
#include <unordered_set>
#include <vector>

// node host 进程的退出条件是 JS 事件循环排空，而每个持有
// Napi::ThreadSafeFunction 的对象（director、异步完成 hook）都持有
// 事件循环引用，其释放原本只能等 native 析构——而析构挂在 GC
// finalizer 上，shutdown 时无人确定性归还，node.exe 便在 IPC 全部
// 清理完后仍不退出（HostLauncher 只能等满 2s 死线强杀）。
// 本注册表把「归还事件循环引用」搬进 shutdown 链路：Core 的
// on_before_shutdown 钩子（GOODBYE / transport 断开 / 父进程退出 /
// RequestStop 四条路径全部前置触发）调用 DetachAll，逐个归还。
class DasNapiDetachableLoopRef
{
public:
    // 析构自动注销：子类析构（引用计数归零或 env teardown）无需额外调用
    virtual ~DasNapiDetachableLoopRef();

    // 幂等且线程安全。实现应 Release 自己的 ThreadSafeFunction。
    // napi 语义：Release 只拒绝新调用，已入队的调用仍会执行。
    virtual void DetachFromLoop() = 0;

    // 子类构造（TSF 创建完成）后调用；若进程已进入停机则立即自 detach，
    // 堵住「shutdown 进行中还有新 loop-ref 诞生」的窗口
    static void Register(DasNapiDetachableLoopRef* holder);
    static void Unregister(DasNapiDetachableLoopRef* holder);

    // shutdown 链路调用：标记停机并逐个 Detach 全部已注册引用。
    // 幂等：多次触发（四条路径互斥触发或重入）只有首次生效。
    static void DetachAll();

    static bool ShuttingDown();

private:
    struct State
    {
        std::mutex                                    mutex;
        std::unordered_set<DasNapiDetachableLoopRef*> holders;
        bool                                          shutting_down{false};
    };

    static State& SharedState();
};

inline DasNapiDetachableLoopRef::~DasNapiDetachableLoopRef()
{
    Unregister(this);
}

inline DasNapiDetachableLoopRef::State& DasNapiDetachableLoopRef::SharedState()
{
    static State state;
    return state;
}

inline void DasNapiDetachableLoopRef::Register(DasNapiDetachableLoopRef* holder)
{
    if (holder == nullptr)
    {
        return;
    }
    State& state = SharedState();
    {
        const std::lock_guard<std::mutex> lock(state.mutex);
        state.holders.insert(holder);
    }
    // 注册与停机标记存在竞争窗口：注册后再查一次标记，
    // 保证停机期间诞生的持有者立刻归还引用
    if (ShuttingDown())
    {
        holder->DetachFromLoop();
    }
}

inline void DasNapiDetachableLoopRef::Unregister(
    DasNapiDetachableLoopRef* holder)
{
    if (holder == nullptr)
    {
        return;
    }
    State&                            state = SharedState();
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.holders.erase(holder);
}

inline void DasNapiDetachableLoopRef::DetachAll()
{
    State&                                 state = SharedState();
    std::vector<DasNapiDetachableLoopRef*> snapshot;
    {
        const std::lock_guard<std::mutex> lock(state.mutex);
        state.shutting_down = true;
        snapshot.assign(state.holders.begin(), state.holders.end());
    }
    // 快照后逐个 detach（DetachFromLoop 幂等，析构竞争也安全），
    // 不持锁调用：TSF Release 线程安全，且避免与子类析构注销交叉
    for (DasNapiDetachableLoopRef* holder : snapshot)
    {
        holder->DetachFromLoop();
    }
}

inline bool DasNapiDetachableLoopRef::ShuttingDown()
{
    State&                            state = SharedState();
    const std::lock_guard<std::mutex> lock(state.mutex);
    return state.shutting_down;
}

#endif // DAS_NAPI_LOOP_DETACH_H
