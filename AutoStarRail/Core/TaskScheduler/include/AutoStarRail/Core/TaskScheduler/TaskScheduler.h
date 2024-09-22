#ifndef ASR_CORE_TASKSCHEDULER_H
#define ASR_CORE_TASKSCHEDULER_H

#include <chrono>
#include <memory>
#include <queue>
#include <thread>
#include <variant>
#include <vector>

#include <AutoStarRail/AsrPtr.hpp>
#include <AutoStarRail/PluginInterface/IAsrTask.h>

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

        std::thread executor_;
        bool        is_not_need_exit_{true};

        // 这里的函数是被优先执行的
        std::mutex                  task_function_queue_mutex_;
        std::queue<TaskFunction>    task_function_queue_;
        std::mutex                  task_queue_mutex_;
        std::vector<SchedulingUnit> task_queue_;

        // 外部传递进来的config
        AsrPtr<IAsrReadOnlyString> config_;
        AsrTask                    last_task_{AsrPtr<IAsrTask>{}};
        AsrReadOnlyString          last_task_execute_message_{};

    public:
        TaskScheduler();
        ~TaskScheduler() = default;

        void SendTask(const TaskFunction& task);
        void UpdateConfig(const AsrReadOnlyString& config);

    private:
        void AddTask(SchedulingUnit task);
        void DeleteTask(SchedulingUnit task);

        void SetErrorMessage(const std::string& message);

        template <class T>
        void DoTask(T* p_task);

        void RunTaskQueue();
        void NotifyExit();
    };
} // namespace Core

ASR_NS_END

#endif // ASR_CORE_TASKSCHEDULER_H