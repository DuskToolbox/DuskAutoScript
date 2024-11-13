#ifndef ASR_CORE_TASKSCHEDULER_H
#define ASR_CORE_TASKSCHEDULER_H

#include "AutoStarRail/Core/ForeignInterfaceHost/TaskManager.h"

#include <chrono>
#include <thread>
#include <vector>

#include <AutoStarRail/AsrPtr.hpp>
#include <AutoStarRail/PluginInterface/IAsrTask.h>

ASR_DISABLE_WARNING_BEGIN
ASR_IGNORE_STDEXEC_PARAMETERS

#include <exec/static_thread_pool.hpp>

ASR_DISABLE_WARNING_END

ASR_NS_BEGIN

namespace Core
{
    class TaskScheduler final : public IAsrTaskScheduler
    {
    public:
        using TaskFunction = std::function<void()>;

        struct SchedulingUnit
        {
            // time_since_epoch
            decltype(std::chrono::system_clock::duration{}
                         .count()) utc_next_run_time;
            AsrPtr<ForeignInterfaceHost::TaskManager::TaskInfo> p_task_info;
            bool operator==(const SchedulingUnit& rhs) const noexcept;

            explicit SchedulingUnit(
                ForeignInterfaceHost::TaskManager::TaskInfo* p_task);
            void RefreshNextRunTime();
        };

    private:
        // 语言VM绑定线程
        exec::static_thread_pool thread_pool{1};

        // 单开一个线程检测是否需要执行任务，是的话调度到thread_pool去
        bool                        is_not_need_exit_{true};
        std::atomic_bool            is_task_working_{false};
        std::mutex                  task_queue_mutex_;
        std::vector<SchedulingUnit> task_queue_;
        std::thread                 executor_;

        // 外部传递进来的config，由前端/后端根据系统实际情况生成
        class EnvironmentConfig
        {
            std::mutex                 mutex_{};
            AsrPtr<IAsrReadOnlyString> environment_config_{};

        public:
            void SetValue(IAsrReadOnlyString* p_config);
            void GetValue(IAsrReadOnlyString** pp_out_config);
        };
        EnvironmentConfig environment_config_;
        AsrPtr<IAsrTask>  last_task_{AsrPtr<IAsrTask>{}};
        AsrReadOnlyString last_task_execute_message_{};

    public:
        TaskScheduler();
        ~TaskScheduler() = default;

        // IAsrBase
        ASR_UTILS_IASRBASE_AUTO_IMPL(TaskScheduler)
        ASR_IMPL QueryInterface(const AsrGuid& iid, void** pp_object) override;
        ASR_IMPL GetAllWorkingTasks(
            IAsrTaskInfoVector** pp_out_task_info_vector) override;
        // IAsrScheduler
        ASR_IMPL AddTask(IAsrTaskInfo* p_task_info) override;
        ASR_IMPL RemoveTask(IAsrTaskInfo* p_task_info) override;
        ASR_IMPL UpdateEnvironmentConfig(
            IAsrReadOnlyString* p_config_json) override;

        void UpdateConfig(const AsrReadOnlyString& config);
        auto GetSchedulerImpl() { return thread_pool.get_scheduler(); }

        AsrResult AddTask(ForeignInterfaceHost::TaskManager::TaskInfo* p_task);
        /**
         * @brief 以json的形式输出当前所有调度任务的信息
         * @param pp_out_json 输出的信息
         * @return ASR_S_OK 成功；其它值表示失败
         */
        AsrResult GetAllTaskSchedulerInfo(IAsrReadOnlyString** pp_out_json);

    private:
        void InternalAddTask(const SchedulingUnit& task);

        void SetErrorMessage(const std::string& message);

        void DoTask(const SchedulingUnit& schedule_unit);

        void RunTaskQueue();
        void NotifyExit();
    };

    extern TaskScheduler g_scheduler;

    void to_json(nlohmann::json& out, const TaskScheduler::SchedulingUnit& in);
} // namespace Core

ASR_NS_END

#endif // ASR_CORE_TASKSCHEDULER_H