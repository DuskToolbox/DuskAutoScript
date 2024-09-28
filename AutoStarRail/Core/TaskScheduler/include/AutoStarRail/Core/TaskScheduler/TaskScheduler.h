#ifndef ASR_CORE_TASKSCHEDULER_H
#define ASR_CORE_TASKSCHEDULER_H

#include <chrono>
#include <memory>
#include <thread>
#include <variant>
#include <vector>

#include <AutoStarRail/AsrPtr.hpp>
#include <AutoStarRail/PluginInterface/IAsrTask.h>

ASR_DISABLE_WARNING_BEGIN
ASR_IGNORE_UNUSED_PARAMETER

#include <exec/static_thread_pool.hpp>

ASR_DISABLE_WARNING_END

ASR_NS_BEGIN

namespace Core
{
    class TaskScheduler : public std::enable_shared_from_this<TaskScheduler>
    {
    public:
        using TaskFunction = std::function<void()>;

    private:
        using AsrTask = std::variant<AsrPtr<IAsrTask>, AsrPtr<IAsrSwigTask>>;
        friend bool operator==(const AsrTask& lhs, const AsrTask& rhs);
        struct SchedulingUnit
        {
            std::chrono::time_point<std::chrono::system_clock> next_run_time;
            AsrTask                                            p_task;

            bool operator==(const SchedulingUnit& rhs);
        };

        // 语言VM绑定线程
        exec::static_thread_pool thread_pool{1};

        // 单开一个线程检测是否需要执行任务，是的话调度到thread_pool去
        bool                        is_not_need_exit_{true};
        std::atomic_bool            is_task_working_{false};
        std::mutex                  task_queue_mutex_;
        std::vector<SchedulingUnit> task_queue_;
        std::thread                 executor_;

        // 外部传递进来的config
        AsrPtr<IAsrReadOnlyString> config_;
        AsrTask                    last_task_{AsrPtr<IAsrTask>{}};
        AsrReadOnlyString          last_task_execute_message_{};

    public:
        TaskScheduler();
        ~TaskScheduler() = default;

        void UpdateConfig(const AsrReadOnlyString& config);
        auto GetSchedulerImpl() { return thread_pool.get_scheduler(); }

        template <class T>
        void AddTask(T* p_task);

    private:
        void AddTask(SchedulingUnit task);
        void DeleteTask(SchedulingUnit task);

        void SetErrorMessage(const std::string& message);

        template <class T>
        void DoTask(T* p_task);

        void RunTaskQueue();
        void NotifyExit();
    };

    extern TaskScheduler g_scheduler;
} // namespace Core

ASR_NS_END

#endif // ASR_CORE_TASKSCHEDULER_H