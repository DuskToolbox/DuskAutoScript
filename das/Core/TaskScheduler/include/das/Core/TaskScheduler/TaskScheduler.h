#ifndef DAS_CORE_TASKSCHEDULER_H
#define DAS_CORE_TASKSCHEDULER_H

#include "das/Core/ForeignInterfaceHost/TaskManager.h"
#include "das/Core/Utils/IDasStopTokenImpl.h"

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include <das/DasPtr.hpp>
#include <das/PluginInterface/IDasTask.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_STDEXEC_PARAMETERS

#include <exec/static_thread_pool.hpp>

DAS_DISABLE_WARNING_END

DAS_NS_BEGIN

namespace Core
{
    class TaskScheduler final : public IDasTaskScheduler
    {
    public:
        struct SchedulingUnit
        {
            // time_since_epoch
            decltype(std::chrono::system_clock::duration{}
                         .count()) utc_next_run_time;
            DasPtr<ForeignInterfaceHost::TaskManager::TaskInfo> p_task_info;
            bool operator==(const SchedulingUnit& rhs) const noexcept;

            explicit SchedulingUnit(
                ForeignInterfaceHost::TaskManager::TaskInfo* p_task);
            void RefreshNextRunTime();
        };

    private:
        // 语言VM绑定线程
        exec::static_thread_pool vm_thread_pool_{8};

        // 单开一个线程检测是否需要执行任务，是的话调度到thread_pool去
        bool                        is_not_need_exit_{true};
        std::atomic_bool            is_profile_enabled_{true};
        std::mutex                  task_queue_mutex_;
        std::vector<SchedulingUnit> task_queue_;
        std::thread                 executor_;

        // 外部传递进来的config，由前端/后端根据系统实际情况生成
        class EnvironmentConfig
        {
            std::mutex                 mutex_{};
            DasPtr<IDasReadOnlyString> environment_config_{};

        public:
            void SetValue(IDasReadOnlyString* p_config);
            void GetValue(IDasReadOnlyString** pp_out_config);
        };
        EnvironmentConfig       environment_config_;
        DasPtr<IDasTask>        last_task_{DasPtr<IDasTask>{}};
        DasReadOnlyString       last_task_execute_message_{};
        DasPtr<IDasJsonSetting> p_state_json_{};

        // 不受mutex_控制
        class TaskController
        {
            friend TaskScheduler;

            std::mutex                     mutex_{};
            bool                           is_task_working_{false};
            Utils::DasStopTokenImplOnStack stop_token_{};

        public:
            template <class F>
            auto ExecuteAtomically(F&& f)
            {
                std::lock_guard _{mutex_};
                return f(*this);
            }
        };
        TaskController task_controller_{};

    public:
        TaskScheduler();
        ~TaskScheduler();

        // IDasBase
        DAS_UTILS_IDASBASE_AUTO_IMPL(TaskScheduler)
        DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_object) override;
        DAS_IMPL GetAllWorkingTasks(
            IDasTaskInfoVector** pp_out_task_info_vector) override;
        // IDasScheduler
        DAS_IMPL AddTask(IDasTaskInfo* p_task_info) override;
        DAS_IMPL RemoveTask(IDasTaskInfo* p_task_info) override;
        DAS_IMPL UpdateEnvironmentConfig(
            IDasReadOnlyString* p_config_json) override;
        DAS_BOOL_IMPL IsTaskExecuting() override;
        DAS_IMPL      SetEnabled(DasBool enabled) override;
        DAS_BOOL_IMPL GetEnabled() override;
        DAS_IMPL      ForceStart() override;
        DAS_IMPL      RequestStop() override;

        auto GetSchedulerImpl() { return vm_thread_pool_.get_scheduler(); }

        DasResult AddTask(ForeignInterfaceHost::TaskManager::TaskInfo* p_task);
        /**
         * @brief 以json的形式输出当前所有调度任务的信息
         * @param pp_out_json 输出的信息
         * @return DAS_S_OK 成功；其它值表示失败
         */
        DasResult GetAllTaskSchedulerInfo(IDasReadOnlyString** pp_out_json);

        void SetStateJson(IDasJsonSetting& state);

    private:
        void InternalAddTask(const SchedulingUnit& task);

        void SetErrorMessage(const std::string& message);

        void DoTask(const SchedulingUnit& schedule_unit);

        void DumpStateToFile();

        // executor_
        void RunTaskQueue();
        void NotifyExit();
    };

    extern DAS::DasPtr<TaskScheduler> g_scheduler;

    void to_json(nlohmann::json& out, const TaskScheduler::SchedulingUnit& in);

    DasResult InitializeGlobalTaskScheduler();
} // namespace Core

DAS_NS_END

#endif // DAS_CORE_TASKSCHEDULER_H