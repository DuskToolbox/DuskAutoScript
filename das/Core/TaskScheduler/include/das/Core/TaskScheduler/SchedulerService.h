#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/Core/TaskScheduler/RepositoryInvokeCompiler.h>
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
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio/steady_timer.hpp>

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
        // Pointer to the task type record if available
        TaskTypeRecord*                        task_type = nullptr;
        DasPtr<Das::PluginInterface::IDasTask> task_instance;
    };

    class SchedulerService
    {
    public:
        explicit SchedulerService(
            Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>
                ipc_context);

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

        yyjson::value GetTaskAuthoringDocument(
            int64_t              task_id,
            const yyjson::value& request);

        yyjson::value ApplyTaskAuthoringChange(
            int64_t              task_id,
            const yyjson::value& change);

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

    private:
        void      StartTickTimer(std::chrono::steady_clock::duration delay);
        void      OnTick();
        DasResult CreateTaskInstance(
            const TaskTypeRecord&            task_type,
            Das::PluginInterface::IDasTask** pp_out_task);

        // IDasAsyncCallback implementation for PostToBusinessThread dispatch
        class TickCallback final : public IDasAsyncCallback
        {
        public:
            explicit TickCallback(SchedulerService* scheduler)
                : scheduler_(scheduler)
            {
            }

            uint32_t AddRef() override { return ++ref_count_; }

            uint32_t Release() override
            {
                auto count = --ref_count_;
                if (count == 0)
                {
                    delete this;
                }
                return count;
            }

            DasResult QueryInterface(const DasGuid& iid, void** pp) override
            {
                if (iid == DasIidOf<IDasAsyncCallback>())
                {
                    AddRef();
                    *pp = this;
                    return DAS_S_OK;
                }
                return DAS_E_NO_INTERFACE;
            }

            DasResult Do() noexcept override
            {
                if (scheduler_)
                {
                    scheduler_->OnTick();
                }
                return DAS_S_OK;
            }

        private:
            SchedulerService*     scheduler_;
            std::atomic<uint32_t> ref_count_{1};
        };

        /// Find task type by GUID. Returns nullptr if not found.
        TaskTypeRecord* FindTaskType(const DasGuid& task_guid);

        /// Find task instance by id. Returns nullptr if not found.
        TaskInstanceRecord* FindTaskInstance(int64_t task_id);

        /// Find task instance by id (const).
        const TaskInstanceRecord* FindTaskInstance(int64_t task_id) const;

        Repository::Dto::RepositoryAvailabilityDto
        DeriveRepositoryAvailabilityLocked(
            const Repository::Dto::RepositoryEntryDto& entry);

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

        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager_;
        Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>
            ipc_context_;

        mutable std::mutex          mutex_;
        std::atomic<SchedulerState> state_{SchedulerState::Stopped};
        std::condition_variable     cv_;
        std::thread                 disable_thread_;
        bool                        initialized_ = false;

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

        Das::PluginInterface::IDasTask*    current_task_ = nullptr;
        std::vector<std::filesystem::path> loaded_plugin_paths_;

        // Cooperative cancellation token for the currently executing task
        DasPtr<Das::PluginInterface::IDasStopToken> stop_token_;

        std::unique_ptr<boost::asio::steady_timer> tick_timer_;

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
        } state_notify_;
    };

} // namespace Das::Core::TaskScheduler
