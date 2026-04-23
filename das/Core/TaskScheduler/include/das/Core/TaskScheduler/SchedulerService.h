#pragma once

#include <atomic>
#include <condition_variable>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasExport.h>
#include <das/DasPtr.hpp>
#include <das/DasSharedRef.hpp>
#include <das/IDasAsyncCallback.h>
#include <das/IDasSchedulerService.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
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
        DasPtr<Das::PluginInterface::IDasTask> task_instance;
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
        int64_t                    id = 0;
        DasGuid                    task_guid;
        DasGuid                    plugin_guid;
        TaskAvailability           availability = TaskAvailability::Available;
        std::string                unavailability_reason;
        std::optional<std::string> next_execution_time;
        nlohmann::json             properties;
        // Pointer to the task type record if available
        TaskTypeRecord* task_type = nullptr;
    };

    class SchedulerService
    {
    public:
        explicit SchedulerService(
            Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
            Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>
                ipc_context);

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
        nlohmann::json Get();

        /// Add a new task instance by task type GUID. Returns the allocated
        /// instance id via out_task_id.
        DasResult AddTask(const DasGuid& task_guid, int64_t* out_task_id);

        /// Delete a queued task instance by its id.
        DasResult DeleteTask(int64_t task_id);

        /// Update task instance properties (validated against descriptors).
        DasResult UpdateTaskProperties(
            int64_t               task_id,
            const nlohmann::json& properties);

        /// Update scheduler-owned internal properties (nextExecutionTime).
        DasResult UpdateTaskInternalProperties(
            int64_t               task_id,
            const nlohmann::json& internal_props);

        /// Check whether the scheduler has been initialized.
        bool IsInitialized() const { return initialized_; }

    private:
        void StartTickTimer();
        void OnTick();

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

        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager_;
        Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>
            ipc_context_;

        mutable std::mutex          mutex_;
        std::atomic<SchedulerState> state_{SchedulerState::Stopped};
        std::condition_variable     cv_;
        bool                        initialized_ = false;

        // Available task types discovered from loaded manifests
        std::vector<std::unique_ptr<TaskTypeRecord>> task_types_;

        // Ordered queued task instances materialized from profile state
        std::vector<TaskInstanceRecord> task_instances_;

        Das::PluginInterface::IDasTask*    current_task_ = nullptr;
        std::vector<std::filesystem::path> loaded_plugin_paths_;

        std::unique_ptr<boost::asio::steady_timer> tick_timer_;
    };

} // namespace Das::Core::TaskScheduler
