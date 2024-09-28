#include <AutoStarRail/Core/ForeignInterfaceHost/AsrGuid.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/AsrStringImpl.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/Core/TaskScheduler/TaskScheduler.h>
#include <AutoStarRail/Core/Utils/InternalUtils.h>
#include <AutoStarRail/Core/Utils/StdExecution.h>

ASR_NS_BEGIN

namespace Core
{
    bool TaskScheduler::SchedulingUnit::operator==(const SchedulingUnit& rhs)
    {
        return this->p_task == rhs.p_task;
    }

    TaskScheduler::TaskScheduler()
        : executor_{[sp_this = shared_from_this()]
                    {
                        ASR_CORE_LOG_INFO("Task scheduler thread launched.");
                        while (sp_this->is_not_need_exit_)
                        {
                            sp_this->RunTaskQueue();
                            using namespace std::literals;
                            std::this_thread::sleep_for(100ms);
                        }
                        ASR_CORE_LOG_INFO("Task scheduler thread exited.");
                    }}
    {
    }
    void TaskScheduler::AddTask(SchedulingUnit task)
    {
        std::lock_guard _{task_queue_mutex_};

        task_queue_.push_back(task);

        std::sort(
            ASR_FULL_RANGE_OF(task_queue_),
            [](const SchedulingUnit& lhs, const SchedulingUnit& rhs)
            { return lhs.next_run_time <= rhs.next_run_time; });
    }

    void TaskScheduler::DeleteTask(SchedulingUnit task)
    {
        std::lock_guard _{task_queue_mutex_};

        std::erase(task_queue_, task);

        std::sort(
            ASR_FULL_RANGE_OF(task_queue_),
            [](const SchedulingUnit& lhs, const SchedulingUnit& rhs)
            { return lhs.next_run_time <= rhs.next_run_time; });
    }

    void TaskScheduler::SetErrorMessage(const std::string& message)
    {
        AsrResult create_error_message_result;
        last_task_execute_message_ =
            AsrReadOnlyString::FromUtf8(message, &create_error_message_result);
        if (IsFailed(create_error_message_result))
        {
            ASR_CORE_LOG_ERROR(
                "Can not save error message. Error code = {}",
                create_error_message_result);
            return;
        }
    }

    template <class T>
    void TaskScheduler::DoTask(T* p_task)
    {
        ASR_CORE_LOG_INFO("Enter!");
        bool      is_success{false};
        AsrResult do_error_code{ASR_E_UNDEFINED_RETURN_VALUE};
        last_task_ = {p_task};
        try
        {
            const auto task_name = Utils::GetRuntimeClassNameFrom(p_task);
            const auto guid = Utils::GetGuidFrom(p_task);
            ASR_CORE_LOG_ERROR(
                "Begin run task. Name = {}, guid = {}, code = {}.",
                task_name,
                guid,
                do_error_code);
            const auto do_result = p_task->Do({config_.Get()});
            do_error_code = GetErrorCodeFrom(do_result);
            if (IsOk(do_result))
            {
                is_success = true;
                const auto message = ASR::fmt::format(
                    "Task execution success. Name = {}, guid = {}, code = {}.",
                    task_name,
                    guid,
                    do_error_code);
                ASR_CORE_LOG_ERROR(message);
                SetErrorMessage(message);
                return;
            }

            // failed
            const auto error_message = ASR::fmt::format(
                "Task execution failed. Name = {}, guid = {}, code = {}.",
                task_name,
                guid,
                do_error_code);
            ASR_CORE_LOG_ERROR(error_message);

            AsrReadOnlyString task_error_message{};
            if constexpr (std::is_same_v<T, IAsrTask>)
            {
                AsrPtr<IAsrReadOnlyString> p_task_error_message{};
                const auto                 create_task_error_message_result =
                    ::AsrGetErrorMessage(
                        p_task,
                        do_error_code,
                        p_task_error_message.Put());
                if (IsFailed(create_task_error_message_result))
                {
                    ASR_CORE_LOG_ERROR(
                        "Get task error message failed. Error code = {}.",
                        create_task_error_message_result);
                    SetErrorMessage(error_message);
                    return;
                }
                task_error_message = p_task_error_message;
            }
            else
            {
                const auto create_task_error_message_result =
                    ::AsrGetErrorMessage(p_task, do_error_code);
                if (IsFailed(create_task_error_message_result))
                {
                    ASR_CORE_LOG_ERROR(
                        "Get task error message failed. Error code = {}.",
                        GetErrorCodeFrom(create_task_error_message_result));
                    SetErrorMessage(error_message);
                    return;
                }
                task_error_message = create_task_error_message_result.value;
            }

            const auto full_error_message = ASR::fmt::format(
                "{}\nMessage from task = \"{}\"",
                error_message,
                task_error_message.GetUtf8());
            SetErrorMessage(full_error_message);
        }
        catch (const AsrException& ex)
        {
            ASR_CORE_LOG_EXCEPTION(ex);
            std::string message;
            if (is_success)
            {
                message = ASR::fmt::format(
                    "Task execution success. Code = {}.",
                    do_error_code);
            }
            else
            {
                message = ASR::fmt::format(
                    "Task execution failed. Error code = {}.",
                    do_error_code);
            }

            ASR_CORE_LOG_ERROR(message);
            SetErrorMessage(message);
        }
        catch (const std::runtime_error& ex)
        {
            ASR_CORE_LOG_EXCEPTION(ex);
            if (do_error_code == ASR_E_UNDEFINED_RETURN_VALUE)
            {
                do_error_code = ASR_E_INTERNAL_FATAL_ERROR;
            }
            SetErrorMessage(ex.what());
        }
    }

    template <class T>
    void TaskScheduler::AddTask(T* p_task)
    {
        ASR_CORE_LOG_INFO("Enter!");
        bool      is_success{false};
        AsrResult do_error_code{ASR_E_UNDEFINED_RETURN_VALUE};
        try
        {
            const auto task_name = Utils::GetRuntimeClassNameFrom(p_task);
            const auto guid = Utils::GetGuidFrom(p_task);

            AsrDate date{};
            if constexpr (ForeignInterfaceHost::is_asr_interface<T>)
            {
                const auto get_date_result =
                    p_task->GetNextExecutionTime(&date);
                if (IsFailed(get_date_result))
                {
                    ASR_CORE_LOG_ERROR(
                        "Can not get next execution time. Task name = {}, guid = {}",
                        task_name,
                        guid);
                    return;
                }
            }
            else
            {
                const auto get_date_result = p_task->GetNextExecutionTime();
                if (IsFailed(get_date_result))
                {
                    ASR_CORE_LOG_ERROR(
                        "Can not get next execution time. Task name = {}, guid = {}",
                        task_name,
                        guid);
                    return;
                }
                date = get_date_result.value;
            }

            is_success = true;
            // 构造一个时间出来
        }
        catch (const AsrException& ex)
        {
            ASR_CORE_LOG_EXCEPTION(ex);
            std::string message;
            if (is_success)
            {
                message = ASR::fmt::format(
                    "Get next execution time success. Code = {}.",
                    do_error_code);
            }
            else
            {
                message = ASR::fmt::format(
                    "Get next execution time success. Error code = {}.",
                    do_error_code);
            }

            ASR_CORE_LOG_ERROR(message);
        }
        catch (const std::runtime_error& ex)
        {
            ASR_CORE_LOG_EXCEPTION(ex);
            if (do_error_code == ASR_E_UNDEFINED_RETURN_VALUE)
            {
                do_error_code = ASR_E_INTERNAL_FATAL_ERROR;
            }
        }
    }

    void TaskScheduler::UpdateConfig(const AsrReadOnlyString& config)
    {
        config_ = config.Get();
    }

    void TaskScheduler::RunTaskQueue()
    {
        if (is_task_working_)
        {
            return;
        }

        SchedulingUnit current_task;
        {
            std::lock_guard _{task_queue_mutex_};
            current_task = task_queue_.back();
            task_queue_.pop_back();
        }

        stdexec::start_on(
            GetSchedulerImpl(),
            stdexec::just()
                | stdexec::then(
                    [sp_this = shared_from_this(), current_task]
                    {
                        sp_this->is_task_working_ = true;
                        std::visit(
                            [sp_this](const auto& task)
                            {
                                sp_this->DoTask(task.Get());
                                {
                                    sp_this->AddTask(task.Get());
                                }
                            },
                            current_task.p_task);
                        sp_this->is_task_working_ = false;
                    }));
    }

    void TaskScheduler::NotifyExit()
    {
        is_not_need_exit_ = false;
        thread_pool.request_stop();
        executor_.detach();
    }

    bool operator==(
        const TaskScheduler::AsrTask& lhs,
        const TaskScheduler::AsrTask& rhs)
    {
        constexpr auto visitor = [](const auto& pointer) -> void*
        { return static_cast<void*>(pointer.Get()); };
        const auto b_lhs = std::visit(visitor, lhs);
        const auto b_rhs = std::visit(visitor, rhs);
        return b_lhs == b_rhs;
    }

    ASR_DEFINE_VARIABLE(g_scheduler);
} // namespace Core

ASR_NS_END