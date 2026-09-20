#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>

#include <das/Core/ForeignInterfaceHost/IPluginLifecycleListener.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/RepositoryInvokeCompiler.h>
#include <das/Core/TaskScheduler/TaskAuthoringContract.h>
#include <das/Core/TaskScheduler/TaskCapabilityRegistry.h>
#include <das/Core/TaskScheduler/TaskRepositoryStore.h>
#include <das/Core/Utils/IDasStopTokenImpl.h>
#include <das/DasExport.h>
#include <das/DasPtr.hpp>
#include <das/DasSharedRef.hpp>
#include <das/IDasAsyncCallback.h>
#include <das/IDasSchedulerService.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio/steady_timer.hpp>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_SML_WARNING
#include <boost/sml.hpp>
DAS_DISABLE_WARNING_END

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_STDEXEC_PARAMETERS
#include <exec/async_scope.hpp>
DAS_DISABLE_WARNING_END

namespace Das::Core::TaskScheduler
{

    // SchedulerState is defined in IDasSchedulerService
    using SchedulerState = IDasSchedulerService::SchedulerState;

    /// Describes an available task type discovered from loaded manifests.
    struct TaskTypeRecord
    {
        DasGuid                    task_guid;
        DasGuid                    plugin_guid;
        std::string                name;
        std::string                description;
        std::optional<std::string> game_name;
        std::vector<Das::Core::ForeignInterfaceHost::PluginSettingDesc>
                                               descriptors;
        uint64_t                               feature_index = 0;
        DasPtr<Das::PluginInterface::IDasTask> prototype_task;
    };

    /// Availability status for a queued task instance.
    enum class TaskAvailability
    {
        Available,
        Unavailable,
        Invalid
    };

    /// Represents a materialized queued task instance in the scheduler runtime.
    struct TaskInstanceRecord
    {
        int64_t                id = 0;
        DasGuid                task_guid;
        DasGuid                plugin_guid;
        TaskAvailability       availability = TaskAvailability::Available;
        std::string            unavailability_reason;
        std::optional<int64_t> next_execution_time;
        yyjson::value          properties;
        yyjson::value          authoring;
        // Per-instance enable switch. Defaults to true for backward
        // compatibility with persisted instances that predate this field.
        // When false the dispatch loop skips this instance without
        // raising errors, but it remains in taskOrder.
        bool enabled = true;
        // Pointer to the task type record if available
        TaskTypeRecord*                        task_type = nullptr;
        DasPtr<Das::PluginInterface::IDasTask> task_instance;
    };

    /// Cached authoring session for one task/repository-entry id. Mirrors the
    /// move-only, non-copyable shape of SettingsManager's SettingsKeyCell
    /// (std::shared_mutex is neither copyable nor movable). The cache map
    /// stores std::shared_ptr<AuthoringSessionEntry> so a caller can hold a
    /// copy, release mutex_, and still use entry->session safely even if the
    /// entry is evicted from the map concurrently. The per-session `lock` is
    /// reserved for per-session serialization (a later plan) and is
    /// intentionally unused here.
    struct AuthoringSessionEntry
    {
        std::shared_mutex                                      lock;
        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
        // Persisted authoring revision the session was built from. Read-side
        // freshness (Get path): if the on-disk revision advanced since creation
        // (an Apply bumped it), the cached session's document view is stale and
        // is rebuilt. -1 = untracked.
        int64_t creation_revision = -1;
        // catalog 构建时的进程 GlobalLocale 快照（原始读数照存照比：读取
        // 失败/未设置时为空串，是合法戳值——两次读数一致即视为 locale 未
        // 变）。与 creation_revision 同类的读侧陈旧性戳：label 是 session
        // 创建时经 context 烤进缓存视图的，locale 变了缓存 session 的
        // label 视图就是旧语言，读侧按戳失效重建。
        std::string creation_locale;
        // 批量 apply 中途失败的回滚墓碑：置位者持有 entry 写锁；命中者
        // （在逐出前已解析到本 entry 的并发 Apply 写锁等待者）不得继续
        // 使用其 session——内存文档带着未持久化的半批突变，必须从持久层
        // 重建。读侧只需 Apply 写路径复查；Get/Compile 读路径容忍一次
        // 瞬态旧视图（下次访问自然重建）。
        bool invalidated_by_rollback = false;

        AuthoringSessionEntry() = default;
        AuthoringSessionEntry(AuthoringSessionEntry&& other) noexcept
            : session(std::move(other.session)),
              creation_revision(other.creation_revision),
              creation_locale(std::move(other.creation_locale)),
              invalidated_by_rollback(other.invalidated_by_rollback)
        {
        }
        AuthoringSessionEntry& operator=(AuthoringSessionEntry&& other) noexcept
        {
            if (this != &other)
            {
                session = std::move(other.session);
                creation_revision = other.creation_revision;
                creation_locale = std::move(other.creation_locale);
                invalidated_by_rollback = other.invalidated_by_rollback;
            }
            return *this;
        }
        AuthoringSessionEntry(const AuthoringSessionEntry&) = delete;
        AuthoringSessionEntry& operator=(const AuthoringSessionEntry&) = delete;
    };

    class SchedulerService
        : public Das::Core::ForeignInterfaceHost::IPluginLifecycleListener
    {
    public:
        explicit SchedulerService(
            Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
            Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context);

        ~SchedulerService();

        /// Initialize scheduler: load plugins, discover task types,
        /// materialize persisted task instances. Must be called before
        /// Start(). Rejects if currently Running with DAS_E_TASK_WORKING.
        DasResult Initialize(
            const std::filesystem::path& plugin_dir,
            const std::vector<DasGuid>&  disabled_guids);

        /// Start the scheduler tick loop. Requires initialized state with
        /// at least one available task instance.
        DasResult Enable();

        /// Stop the scheduler, cancel timer, wait for current task,
        /// unload plugins, clear runtime state.
        DasResult Disable();

        SchedulerState Status() const;

        /// Returns the merged scheduler state as lower camelCase JSON.
        yyjson::value Get();

        /// Add a new task instance by task type GUID. Returns the allocated
        /// instance id via out_task_id.
        DasResult AddTask(const DasGuid& task_guid, int64_t* out_task_id);

        /// Delete a queued task instance by its id.
        DasResult DeleteTask(int64_t task_id);

        /// Update task instance properties (validated against descriptors).
        DasResult UpdateTaskProperties(
            int64_t              task_id,
            const yyjson::value& properties);

        /// Update scheduler-owned internal properties (nextExecutionTime).
        DasResult UpdateTaskInternalProperties(
            int64_t              task_id,
            const yyjson::value& internal_props);

        /// Toggle the per-instance enable switch and persist it. Allowed in
        /// any scheduler state so a running scheduler can skip/restore a
        /// task without a full stop. Unknown task_id returns DAS_E_NOT_FOUND.
        DasResult SetTaskEnabled(int64_t task_id, bool enabled);

        /// 立即执行一个已物化的 task 实例：投递单任务执行操作
        /// （ExecuteTaskOnce），点谁跑谁、跑完即止——不进调度链（不经
        /// 全局选任务、不触发排空），其他任务的到期残留不被连带清算。
        /// 仅 Stopped 且执行槽空闲时允许；Running/Stopping 或手动执行中
        /// 返回 DAS_E_TASK_WORKING。投递后立即返回，Do 在 BusinessThread
        /// 串行执行；落地时锁内复检 scheduler 状态与任务可用性，投递
        /// 窗口内被 Enable 抢先/任务被删或禁用则作废。
        DasResult RunTaskNow(int64_t task_id);

        yyjson::value GetTaskAuthoringDocument(
            int64_t              task_id,
            const yyjson::value& request);

        yyjson::value ApplyTaskAuthoringChange(
            int64_t              task_id,
            const yyjson::value& change);

        // 计算含 authoring capability 的 task 的首次 next_execution_time。
        // 用途：AddTask 时算首次时间（首跑语义 B），ApplyTaskAuthoringChange
        // 成功后重算。返回 disengaged 表示无 authoring capability / Compile /
        // GetNextExecutionTime 任一失败——caller 保持既有 next_execution_time
        // 不变。task_ptr 必须非空（caller 持有 inst 引用，scheduler idle guard
        // 保证不被释放）。
        std::optional<int64_t> ComputeInitialNextExecutionTime(
            const DasGuid&                  task_guid,
            int64_t                         task_id,
            const yyjson::value&            properties,
            const yyjson::value&            task_json,
            Das::PluginInterface::IDasTask* task_ptr);

        /// 用最新 schedule 重算所有 task 的 next_execution_time。Enable 锁外段
        /// 调用，覆盖非 Running 态 apply 的 schedule 变更与 off→daily 残留的
        /// sentinel。不持 mutex_（内部 ComputeInitialNextExecutionTime
        /// 自管锁）。
        void RefreshNextExecutionTimes();

        yyjson::value CompileTaskAuthoring(
            int64_t              task_id,
            const yyjson::value& request);

        yyjson::value GetRepositoryEntryAuthoringDocument(
            int64_t              entry_id,
            const yyjson::value& request);

        yyjson::value ApplyRepositoryEntryAuthoringChange(
            int64_t              entry_id,
            const yyjson::value& change);

        yyjson::value CompileRepositoryEntryAuthoring(
            int64_t              entry_id,
            const yyjson::value& request);

        yyjson::value GetTaskRepository();
        yyjson::value CreateRepositoryEntry(const yyjson::value& request);
        DasResult     DeleteRepositoryEntry(int64_t entry_id);
        yyjson::value RenameRepositoryEntry(
            int64_t              entry_id,
            const yyjson::value& request);

        std::optional<DasGuid> FindTaskExecutionComponent(
            const DasGuid& task_guid) const;

        RepositoryInvoke::RepositoryInvokeCompileResult
        ResolveRepositoryInvokeSnapshot(
            const RepositoryInvoke::Dto::RepositoryTaskRefDto& repository_ref);

        RepositoryInvoke::RepositoryInvokeCompileResult
        ResolveRepositoryInvokeSnapshot(
            const RepositoryInvoke::Dto::RepositoryTaskRefDto& repository_ref,
            const RepositoryInvoke::RepositoryInvokeSourceContext&
                source_context);

        /// Check whether the scheduler has been initialized.
        bool IsInitialized() const { return initialized_; }

        /// Register a callback to be invoked when scheduler state changes.
        void SetStateNotifyCallback(SchedulerNotifyFunc func, void* user_data);

        /// Drop every cached authoring session (task + repository). Called when
        /// the plugin runtime is invalidated (e.g. plugin unload/reload) so the
        /// next authoring access rebuilds against fresh plugin state. Holds
        /// mutex_ only to clear the maps; in-flight callers keep their entries
        /// alive via shared_ptr (D-87-07).
        void InvalidateAuthoringSessions();

        // IPluginLifecycleListener (D-87-08): PluginManager 在释 mutex_ 后回调
        // 本方法（参考其 Shutdown 两阶段范式）。粗粒度全清 —— 不区分
        // Loaded/Unloading 也不区分哪个 plugin，任何 plugin 变动都全清
        // authoring cache，下次 Get 重建。本方法不重 acquire
        // PluginManager::mutex_（避免反向锁序；listener 触发时 PluginManager
        // 已不持 mutex_）。public 以便测试直接调用 listener 入口验证调用链。
        void OnPluginLifecycleEvent(
            Das::Core::ForeignInterfaceHost::PluginLifecyclePhase phase,
            const DasGuid& plugin_guid) override;

    private:
        void InvalidateAuthoringSessionsLocked();
        void StartTickTimer(std::chrono::steady_clock::duration delay);
        void OnTick();

        /// RunTaskNow 投递的单任务执行落地体：锁内复检 scheduler 状态
        /// （仅 Stopped 执行，与 OnTick 的 Running 专属互斥，杜绝调度与
        /// 手动双通路并发）与任务可用性（投递窗口内被删/禁用作废），
        /// 再交共享执行体。BusinessThread 串行执行。
        void ExecuteTaskOnce(int64_t task_id);

        /// OnTick 与 ExecuteTaskOnce 的共享执行体：envelope 编译 / Do /
        /// GetNextExecutionTime / 执行槽复位 / 状态机收尾（Running 重排
        /// timer、Stopping 完成转换）/ 持久化 / 状态推送。调用前调用方
        /// 已在锁内占好执行槽（current_task_id_）并拷出执行数据。
        void ExecuteSelectedTask(
            int64_t                                     selected_id,
            Das::DasPtr<Das::PluginInterface::IDasTask> task_holder,
            yyjson::value                               properties_copy,
            std::optional<TaskAuthoringCapability>      authoring_capability);

        /// 将一次 tick 投递到 BusinessThread，经 scope_ 跟踪生命周期。
        /// tick 生产点唯一：timer 到期回调。析构以 on_empty 排空全部
        /// 在飞 tick 后才关 persist 队列。
        void SpawnTick();

        /// 将一次单任务执行投递到 BusinessThread（RunTaskNow 唯一
        /// 出口），经 scope_ 跟踪生命周期，与 SpawnTick 同型异常兜底。
        void SpawnExecuteOnce(int64_t task_id);

        DasResult CreateTaskInstance(
            const TaskTypeRecord&            task_type,
            Das::PluginInterface::IDasTask** pp_out_task);

        /// Repository-invoke compile services backing the public snapshot
        /// resolver (entry resolution for settings-driven invocation nodes).
        RepositoryInvoke::RepositoryInvokeCompileServices
        BuildRepositoryInvokeCompileServices();

        /// Find task type by GUID. Returns nullptr if not found.
        TaskTypeRecord* FindTaskType(const DasGuid& task_guid);

        /// Find task instance by id. Returns nullptr if not found.
        TaskInstanceRecord* FindTaskInstance(int64_t task_id);

        /// Find task instance by id (const).
        const TaskInstanceRecord* FindTaskInstance(int64_t task_id) const;

        Repository::Dto::RepositoryAvailabilityDto
        DeriveRepositoryAvailabilityLocked(
            const Repository::Dto::RepositoryEntryDto& entry);

        // Authoring guards — caller must hold mutex_ (*Locked suffix).
        // Returns engaged optional on guard failure; nullopt on pass.
        std::optional<AuthoringGuardError> CheckSchedulerIdleLocked() const;

        /// 统一 busy 判定：非 Stopped（Running/Stopping）或手动立即执行
        /// 占用执行槽（current_task_id_ 非零）都算忙。所有会变更
        /// task_instances_ 或触碰插件 IPC 的入口都应拒绝 busy 状态。
        /// caller 必须持有 mutex_。
        bool SchedulerBusyLocked() const;

        std::optional<AuthoringGuardError> ResolveAuthoringCapabilityLocked(
            const DasGuid&           task_guid,
            TaskAuthoringCapability& out_capability);

        /// 组件文档口面预备 pass：图会话 context 装配时（Get/Apply/Compile
        /// 管线外圈、锁外）扫图文档节点，把组件文档的口面快照现算注入
        /// context 的 nodeFaces/entryFaces 键。瞬态现算零持久化零新缓存
        /// ——绑条目节点复用既有条目会话缓存（GetOrCreateAuthoringSession，
        /// 按 revision+locale 失效），未绑 componentRef 节点（含 begin/end，
        /// 经注册面反查命中既有 QI 链，无特判）现建临时会话。无该 section
        /// 或无 authoring capability 的节点跳过不产诊断；非图文档（无
        /// nodes 数组）直接返回，context 保持现状逐字节一致。host_id 用于
        /// 自引用防护：绑条目等于宿主 id 时跳过（否则宿主会话锁自死锁）。
        /// 调用位必须在 mutex_ 短锁段之外（内部会短暂取 mutex_ 做注册表
        /// 反查与条目会话缓存 resolve）。
        ///
        /// 返回值 = 宿主会话缓存的有效 revision 戳：faces 经 context 只在
        /// 会话创建时播种进 Core 会话状态，而宿主（图）会话缓存按宿主自身
        /// revision 失效——条目 revision 推进若不反映到宿主会话的失效键，
        /// 缓存会话会带着旧 faces 续命（跨文档陈旧）。本 pass 把宿主
        /// revision 与全部绑条目 revision 混合成单一戳返回，调用位以它作
        /// GetOrCreateAuthoringSession 的 current_revision——缓存仍是纯
        /// revision 键控（零新缓存层），任一被依赖条目 revision 变化即重
        /// 建宿主会话（faces 与文档快照同 freshness）。节点口面源自身是宿
        /// 主文档的一部分（节点 settings），宿主 revision 已覆盖。非图文档
        /// 返回宿主 revision 原值。
        int64_t AppendComponentFacesContextKeys(
            yyjson::value&       context,
            const yyjson::value& properties,
            int64_t              host_id);

        /// Get-or-create a cached authoring session for the given id. Returns a
        /// shared_ptr copy (refcount+1) so the caller can release mutex_ before
        /// using entry->session; the entry stays alive even if evicted from the
        /// map concurrently. mutex_ is held only for the short find/emplace;
        /// session creation (factory lookup + plugin IPC) happens outside the
        /// lock. Returns null on session-create failure (no empty entry is
        /// inserted into the map).
        ///
        /// Read-side freshness: when enforce_fresh is true (Get path), a cached
        /// entry whose creation_revision differs from current_revision is
        /// dropped and rebuilt, so a Get following an Apply observes the new
        /// persisted revision. Apply passes enforce_fresh=false (it derives the
        /// base revision from the change, not the session view) so consecutive
        /// Applies reuse one session.
        std::shared_ptr<AuthoringSessionEntry> GetOrCreateAuthoringSession(
            std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>&
                                           cache,
            const TaskAuthoringCapability& capability,
            int64_t                        id,
            yyjson::value                  context,
            int64_t                        current_revision = -1,
            bool                           enforce_fresh = false);

        /// Peek the cache without creating. Returns the cached entry shared_ptr
        /// (refcount+1) or null. The Apply path uses this to distinguish cache
        /// hit (revision check inside the per-session write lock) from miss
        /// (revision check before session create, preserving D-85-02c).
        std::shared_ptr<AuthoringSessionEntry> FindCachedAuthoringSession(
            std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>&
                    cache,
            int64_t id);

        /// Evict a single cached session by id. Called with mutex_ held;
        /// performs a direct map.erase (no drain). In-flight callers that hold
        /// a shared_ptr copy keep their entry alive until they finish
        /// (D-87-07 shared_ptr upgrade roots out the UAF that a drain cycle
        /// was designed to prevent).
        void EvictAuthoringSession(
            std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>&
                    cache,
            int64_t id);

        // ----------------------------------------------------------------
        // Config-side persistence (OnTick nextExecutionTime)
        // ----------------------------------------------------------------

        /**
         * @brief Persistence request produced by BusinessThread runtime.
         *
         * Produced after OnTick updates in-memory nextExecutionTime.
         * Consumed by the config persistence thread, which calls
         * SettingsManager::UpdateTaskInstanceJson. Failures are logged
         * and do not roll back runtime state; the next OnTick republishes
         * a fresh value.
         *
         * @architecture Phase 52 BusinessThread runtime domain -> Settings
         * config domain. SchedulerService::mutex_ must NOT be held when
         * the config persist thread processes these events.
         */
        struct ConfigPersistEvent
        {
            int64_t task_id;
            int64_t next_execution_time;
        };

        /// Post a persistence event for the config-side thread.
        void PostPersistEvent(int64_t task_id, int64_t next_time);

        /// Background loop: waits on CV, drains queue, calls SettingsManager.
        void ConfigPersistThreadLoop();

        /// Signal shutdown, drain remaining events, join the thread.
        /// Must be called before SettingsManager dependencies are destroyed.
        void ShutdownConfigPersistQueue();

        /// 序列化当前 scheduler 状态，经 state_notify_ 推
        /// WS（api/v1/scheduler/get）。 Enable/Disable/OnTick
        /// 状态变化时调用，让 UI 实时收 Running/Stopping/Stopped。
        /// 调用者不得持有 mutex_：内部 Get() 会自加锁，持锁调用会递归死锁。
        /// 故 FSM action 不直接调用，由 Enable/Disable/OnTick 在锁外触发。
        void NotifyStateChange();

        struct EvtEnable
        {
        };
        struct EvtDisable
        {
        };
        struct EvtTickCompleted
        {
        };
        struct FsmDefinition
        {
            auto operator()();
        };
        friend struct FsmDefinition;
        bool FsmGuardNotInitialized() const;
        bool FsmGuardNoAvailableTask() const;
        void FsmActEnterRunning();
        void FsmActEnterStopping();
        void FsmActEnterStopped();
        void FsmActRejectNotInitialized();
        void FsmActRejectNoTask();
        void FsmActRejectFail();
        void FsmActRejectIgnored();

        Das::Core::ForeignInterfaceHost::PluginManager&       plugin_manager_;
        Das::DasRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context_;
        // BusinessThread 投递通道（tick 唯一执行线程）。声明序必须在
        // ipc_context_ 之后：初始化列表依赖 ipc_context_.get()。
        Das::Core::IPC::BusinessThreadScheduler<
            DAS::Core::IPC::MainProcess::IIpcContext>
            bt_scheduler_;

        mutable std::mutex          mutex_;
        std::atomic<SchedulerState> state_{SchedulerState::Stopped};
        bool                        initialized_ = false;
        std::unique_ptr<boost::sml::sm<FsmDefinition>> fsm_;
        DasResult fsm_last_result_ = DAS_S_OK;

        // Available task types discovered from loaded manifests
        std::vector<std::unique_ptr<TaskTypeRecord>> task_types_;
        TaskCapabilityRegistry                       capability_registry_;

        // Ordered queued task instances materialized from profile state
        std::vector<TaskInstanceRecord> task_instances_;

        std::shared_ptr<TaskRepositoryStore> task_repository_store_;

        struct RepositoryPluginAvailabilityState
        {
            std::string reason;
            std::string message;
        };
        std::unordered_map<DasGuid, RepositoryPluginAvailabilityState>
            repository_plugin_availability_;

        // Cached authoring sessions, keyed by task/repository-entry id. Both
        // maps are guarded by mutex_ (short find/emplace/erase only; session
        // use and persistence happen outside the lock). Values are shared_ptr
        // so callers hold a copy after releasing mutex_.
        std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>
            task_authoring_sessions_;
        std::unordered_map<int64_t, std::shared_ptr<AuthoringSessionEntry>>
            repository_authoring_sessions_;

        // True while SchedulerService has already invalidated the authoring
        // caches under mutex_ and is actively changing plugins. Lifecycle
        // callbacks may arrive synchronously or from Host threads during this
        // window; they must not try to acquire mutex_ again.
        std::atomic<bool> suppress_plugin_lifecycle_invalidation_{false};

        // 执行槽占用标记：disengaged = 空闲，engaged 值 = 正在执行的
        // task id。用 optional 而非 0 哨兵——task id 0 是合法 id（测试与
        // 持久化数据均使用），0 会被误判为空闲。OnTick（调度 tick）与
        // ExecuteTaskOnce（手动单任务）两条通路各自锁内置位，共享执行体
        // ExecuteSelectedTask 收尾重锁复位。不持有任何任务指针——执行
        // 期间的对象由通路栈上 DasPtr 拷贝保活，写回经
        // FindTaskInstance(id) 现查。Get() 以 runningTaskId 输出。
        std::optional<int64_t>             current_task_id_;
        std::vector<std::filesystem::path> loaded_plugin_paths_;

        // Cooperative cancellation token for the currently executing task
        DasPtr<Das::PluginInterface::IDasStopToken> stop_token_;

        std::unique_ptr<boost::asio::steady_timer> tick_timer_;

        // tick 生命周期账本：与 tick_timer_ 同组，所有在飞 tick 都在其中。
        // 析构先排空本 scope 再关 persist 消费队列——顺序即「最后一条
        // persist 事件不丢」的修复核心（先关消费者会丢事件）。
        exec::async_scope scope_;

        // BT 上 Disable 泵化排空的完成信号：on_empty 落地时由 detached
        // continuation 置位，泵循环只读它（PumpUntilPredicate 的 is_done
        // 契约：只读 atomic/已同步状态）。scope 已空时订阅即同步完成，
        // 首轮谓词即命中。
        std::atomic<bool> drain_complete_{false};

        // Config-side persistence queue for OnTick nextExecutionTime
        std::queue<ConfigPersistEvent> config_persist_queue_;
        mutable std::mutex             config_persist_mutex_;
        std::condition_variable        config_persist_cv_;
        std::thread                    config_persist_thread_;
        std::atomic<bool>              config_persist_shutdown_{false};

        struct StateNotify
        {
            SchedulerNotifyFunc func = nullptr;
            void*               user_data = nullptr;

            void operator()(const char* json) const { func(json, user_data); }
            explicit operator bool() const { return func != nullptr; }
        };

        // {func,user_data} 双字回调的发布快照（std::atomic<std::shared_ptr>
        // 在本工具链 libc++ 未实现，用专用小锁 + shared_ptr 等价落地）：
        // 写侧锁内整体发布，读侧锁内拷贝快照后锁外调用，消除运行期重设
        // 回调时撕裂读（新 func + 旧 user_data 或半写指针）的窗口。
        mutable std::mutex           state_notify_mutex_;
        std::shared_ptr<StateNotify> state_notify_;
    };

} // namespace Das::Core::TaskScheduler
