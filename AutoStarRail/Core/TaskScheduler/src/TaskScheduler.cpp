#include <AutoStarRail/Core/ForeignInterfaceHost/AsrGuid.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/AsrStringImpl.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/Core/TaskScheduler/TaskScheduler.h>
#include <AutoStarRail/Core/Utils/InternalUtils.h>

ASR_NS_BEGIN

namespace Core
{
    bool TaskScheduler::SchedulingUnit::operator==(const SchedulingUnit& rhs)
    {
        return this->p_task == rhs.p_task;
    }

    TaskScheduler::TaskScheduler()
        : executor_{
              [sp_this = shared_from_this()]
              {
                  ASR_CORE_LOG_INFO("Task scheduler thread launched.");
                  while (sp_this->is_not_need_exit_)
                  {
                      bool is_function_queue_empty = false;
                      while (true)
                      {
                          sp_this->task_function_queue_mutex_.lock();
                          is_function_queue_empty =
                              sp_this->task_function_queue_.empty();
                          if (is_function_queue_empty)
                          {
                              sp_this->task_function_queue_mutex_.unlock();
                              break;
                          }
                          const auto function =
                              sp_this->task_function_queue_.front();
                          sp_this->task_function_queue_.pop();
                          is_function_queue_empty =
                              sp_this->task_function_queue_.empty();

                          sp_this->task_function_queue_mutex_.unlock();

                          function();
                      }
                      sp_this->RunTaskQueue();

                      if (is_function_queue_empty)
                      {
                          using namespace std::literals;
                          std::this_thread::sleep_for(1ms);
                      }
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
        last_task_ = {p_task};
        const auto do_result = p_task->Do({config_.Get()});
        const auto do_error_code = GetErrorCodeFrom(do_result);
        if (IsOk(do_result))
        {
            try
            {
                const auto task_name = Utils::GetRuntimeClassNameFrom(p_task);
                const auto guid = Utils::GetGuidFrom(p_task);
                const auto message = ASR::fmt::format(
                    "Task execution success. Task name = {}, task guid = {}, code = {}.",
                    task_name,
                    guid,
                    do_error_code);
                ASR_CORE_LOG_ERROR(message);
                SetErrorMessage(message);
            }
            catch (const AsrException& ex)
            {
                ASR_CORE_LOG_EXCEPTION(ex);
                const auto message = ASR::fmt::format(
                    "Task execution success. Code = {}.",
                    do_error_code);
                SetErrorMessage(message);
            }
            return;
        }

        try
        {
            const auto task_name = Utils::GetRuntimeClassNameFrom(p_task);
            const auto guid = Utils::GetGuidFrom(p_task);
            const auto error_message = ASR::fmt::format(
                "Task execution failed. Task name = {}, task guid = {}, error code = {}.",
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
                        "Get task error message failed. Error code = {}",
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
                        "Get task error message failed. Error code = {}",
                        GetErrorCodeFrom(create_task_error_message_result));
                    SetErrorMessage(error_message);
                    return;
                }
                task_error_message = create_task_error_message_result.value;
            }

            const auto full_error_message = ASR::fmt::format(
                "{}\n{}",
                error_message,
                task_error_message.GetUtf8());
            SetErrorMessage(full_error_message);
        }
        catch (const AsrException& ex)
        {
            ASR_CORE_LOG_EXCEPTION(ex);

            const auto message = ASR::fmt::format(
                "Task execution failed. Error code = {}.",
                do_error_code);
            ASR_CORE_LOG_ERROR(message);

            SetErrorMessage(message);
        }
        catch (const std::exception& ex)
        {
            ASR_CORE_LOG_EXCEPTION(ex);
        }
    }

    void TaskScheduler::SendTask(const TaskFunction& task)
    {
        task_function_queue_.push(task);
    }

    void TaskScheduler::UpdateConfig(const AsrReadOnlyString& config)
    {
        config_ = config.Get();
    }

    void TaskScheduler::RunTaskQueue()
    {
        SchedulingUnit current_task;
        {
            std::lock_guard _{task_queue_mutex_};
            current_task = task_queue_.back();
            task_queue_.pop_back();
        }

        std::visit(
            [this](const auto& task) { DoTask(task.Get()); },
            current_task.p_task);
    }

    void TaskScheduler::NotifyExit()
    {
        is_not_need_exit_ = false;
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
} // namespace Core

ASR_NS_END