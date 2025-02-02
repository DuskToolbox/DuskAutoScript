#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/TaskScheduler/TaskScheduler.h>
#include <das/Core/Utils/IDasStopTokenImpl.h>
#include <das/Core/Utils/InternalUtils.h>
#include <das/Core/Utils/StdExecution.h>
#include <das/Utils/ThreadUtils.h>

DAS_NS_BEGIN

namespace Core
{
    DAS_NS_ANONYMOUS_DETAILS_BEGIN

    time_t ToUTC(std::tm& time_info) noexcept
    {
#ifdef _WIN32
        std::time_t tt = _mkgmtime(&time_info);
#else
        time_t tt = timegm(&timeinfo);
#endif
        return tt;
    }

    auto CreateDateTime(const DasDate& date) noexcept
    {
        auto time_info1 = tm();
        time_info1.tm_year = date.year - 1900;
        time_info1.tm_mon = date.month - 1;
        time_info1.tm_mday = date.day;
        time_info1.tm_hour = date.hour;
        time_info1.tm_min = date.minute;
        time_info1.tm_sec = date.second;
        tm     time_info = time_info1;
        time_t tt = ToUTC(time_info);
        return tt;
    }

    DAS_NS_ANONYMOUS_DETAILS_END

    bool TaskScheduler::SchedulingUnit::operator==(
        const SchedulingUnit& rhs) const noexcept
    {
        DasGuid iid_lhs;
        DasGuid iid_rhs;
        p_task_info->GetIid(&iid_lhs);
        rhs.p_task_info->GetIid(&iid_rhs);
        return iid_lhs == iid_rhs;
    }

    TaskScheduler::SchedulingUnit::SchedulingUnit(
        ForeignInterfaceHost::TaskManager::TaskInfo* p_task_info)
        : p_task_info{p_task_info}
    {
        RefreshNextRunTime();
    }

    void TaskScheduler::SchedulingUnit::RefreshNextRunTime()
    {
        DasGuid guid;
        p_task_info->GetIid(&guid);
        DasDate    date{};
        const auto get_date_error_code =
            p_task_info->GetTask()->GetNextExecutionTime(&date);
        if (IsFailed(get_date_error_code))
        {
            DasPtr p_name{p_task_info->GetName()};
            DAS_CORE_LOG_ERROR(
                "Can not get next execution time. Task name = {}, guid = {}, error code = {}",
                p_name,
                guid,
                get_date_error_code);
            DAS_THROW_EC(get_date_error_code);
        }
        utc_next_run_time = Details::CreateDateTime(date);
    }

    void TaskScheduler::EnvironmentConfig::SetValue(
        IDasReadOnlyString* p_config)
    {
        std::lock_guard _{mutex_};
        environment_config_ = p_config;
    }

    void TaskScheduler::EnvironmentConfig::GetValue(
        IDasReadOnlyString** pp_out_config)
    {
        std::lock_guard _{mutex_};
        Utils::SetResult(environment_config_, pp_out_config);
    }

    TaskScheduler::TaskScheduler()
        : executor_{
              [sp_this = DasPtr{this}]
              {
                  DAS::Utils::SetCurrentThreadName(L"TaskScheduler COMMAND");
                  using namespace std::literals;
                  DAS_CORE_LOG_INFO("Task scheduler thread launched.");
                  while (sp_this->is_not_need_exit_)
                  {
                      if (!sp_this->is_profile_enabled_)
                      {
                          std::this_thread::sleep_for(100ms);
                          continue;
                      }
                      sp_this->RunTaskQueue();

                      std::this_thread::sleep_for(100ms);
                  }
                  DAS_CORE_LOG_INFO("Task scheduler thread exited.");
              }}
    {
        stdexec::sync_wait(
            stdexec::then(
                stdexec::schedule(GetSchedulerImpl()),
                []
                {
                    DAS_CORE_LOG_INFO("Set thread vm pool thread 1 name.");
                    DAS::Utils::SetCurrentThreadName(L"VM POOL 1");
                }));
    }

    DasResult TaskScheduler::QueryInterface(
        const DasGuid& iid,
        void**         pp_object)
    {
        return Utils::QueryInterface<IDasTaskScheduler>(this, iid, pp_object);
    }

    DAS_NS_ANONYMOUS_DETAILS_BEGIN

    class IDasTaskInfoVectorImpl final : public IDasTaskInfoVector
    {
    public:
        using TaskVector = std::vector<TaskScheduler::SchedulingUnit>;
        IDasTaskInfoVectorImpl(const TaskVector& tasks) : all_tasks{tasks} {}

    private:
        // IDasBase
        DAS_UTILS_IDASBASE_AUTO_IMPL(IDasTaskInfoVectorImpl)
        DAS_IMPL QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            return Utils::QueryInterface<IDasTaskInfoVector>(
                this,
                iid,
                pp_object);
        }
        // IDasTaskInfoVector
        DAS_IMPL EnumByIndex(size_t index, IDasTaskInfo** pp_out_info) override
        {
            DAS_UTILS_CHECK_POINTER(pp_out_info);
            if (index > all_tasks.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }
            Utils::SetResult(all_tasks[index].p_task_info, pp_out_info);
            return DAS_S_OK;
        }
        DasResult EnumNextExecuteTimeByIndex(size_t index, time_t* p_out_time)
            override
        {
            DAS_UTILS_CHECK_POINTER(p_out_time);
            if (index > all_tasks.size())
            {
                return DAS_E_OUT_OF_RANGE;
            }
            *p_out_time = all_tasks[index].utc_next_run_time;
            return DAS_S_OK;
        }

        TaskVector all_tasks;
    };

    DAS_NS_ANONYMOUS_DETAILS_END

    DasResult TaskScheduler::GetAllWorkingTasks(
        IDasTaskInfoVector** pp_out_task_info_vector)
    {
        DasPtr<Details::IDasTaskInfoVectorImpl> p_result;
        {
            std::lock_guard _{task_queue_mutex_};
            p_result = MakeDasPtr<Details::IDasTaskInfoVectorImpl>(task_queue_);
        }
        Utils::SetResult(p_result, pp_out_task_info_vector);
        return DAS_S_OK;
    }

    DasResult TaskScheduler::AddTask(IDasTaskInfo* p_task_info)
    {
        DasPtr<ForeignInterfaceHost::TaskManager::TaskInfo> p_task_info_impl{};
        if (const auto qi_result = p_task_info->QueryInterface(
                DasIidOf<ForeignInterfaceHost::TaskManager::TaskInfo>(),
                p_task_info_impl.PutVoid());
            IsFailed(qi_result))
        {
            DAS_CORE_LOG_ERROR(
                "Can not find class ForeignInterfaceHost::TaskManager::TaskInfo.");
            return qi_result;
        }
        return AddTask(p_task_info_impl.Get());
    }

    DasResult TaskScheduler::RemoveTask(IDasTaskInfo* p_task_info)
    {
        DasPtr<ForeignInterfaceHost::TaskManager::TaskInfo> p_task_info_impl{};
        if (const auto qi_result = p_task_info->QueryInterface(
                DasIidOf<ForeignInterfaceHost::TaskManager::TaskInfo>(),
                p_task_info_impl.PutVoid());
            IsFailed(qi_result))
        {
            DAS_CORE_LOG_ERROR(
                "Can not find class ForeignInterfaceHost::TaskManager::TaskInfo.");
            return qi_result;
        }
        DasGuid target_iid;
        p_task_info->GetIid(&target_iid);
        std::lock_guard _{task_queue_mutex_};
        const auto      it_to_be_removed = std::find_if(
            DAS_FULL_RANGE_OF(task_queue_),
            [target_iid](const SchedulingUnit& v)
            {
                DasGuid v_iid;
                v.p_task_info->GetIid(&v_iid);
                return v_iid == target_iid;
            });
        if (it_to_be_removed == task_queue_.end())
        {
            return DAS_E_OUT_OF_RANGE;
        }
        task_queue_.erase(it_to_be_removed);
        return DAS_S_OK;
    }

    DasResult TaskScheduler::UpdateEnvironmentConfig(
        IDasReadOnlyString* p_config_json)
    {
        DAS_UTILS_CHECK_POINTER(p_config_json);

        environment_config_.SetValue(p_config_json);
        return DAS_S_OK;
    }

    DasBool TaskScheduler::IsTaskExecuting()
    {
        return task_controller_.ExecuteAtomically(
                   [](const TaskController& self)
                   { return self.is_task_working_; })
                   ? DAS_TRUE
                   : DAS_FALSE;
    }

    DasResult TaskScheduler::SetEnabled(DasBool enabled)
    {
        is_profile_enabled_ = enabled;
        return DAS_S_OK;
    }

    DasBool TaskScheduler::GetEnabled()
    {
        return is_profile_enabled_ ? DAS_TRUE : DAS_FALSE;
    }

    DasResult TaskScheduler::ForceStart()
    {
        return task_controller_.ExecuteAtomically(
            [this](TaskController& self)
            {
                if (self.is_task_working_ || !is_profile_enabled_)
                {
                    DAS_CORE_LOG_ERROR("Task is running.");
                    return DAS_E_TASK_WORKING;
                }

                std::lock_guard _{task_queue_mutex_};
                if (task_queue_.empty())
                {
                    task_controller_.ExecuteAtomically(
                        [](TaskController& self)
                        { self.is_task_working_ = false; });
                    return DAS_E_OUT_OF_RANGE;
                }
                auto&      task = task_queue_.back();
                const auto now = std::chrono::system_clock::now();
                time_t     time = std::chrono::system_clock::to_time_t(now);
                task.utc_next_run_time = time;
                return DAS_S_OK;
            });
    }

    DasResult TaskScheduler::RequestStop()
    {
        return task_controller_.ExecuteAtomically(
            [](TaskController& self)
            {
                if (self.is_task_working_)
                {
                    return DAS_E_TASK_WORKING;
                }
                if (self.stop_token_.StopRequested())
                {
                    return DAS_S_FALSE;
                }
                self.stop_token_.RequestStop();
                return DAS_S_OK;
            });
    }

    void TaskScheduler::InternalAddTask(const SchedulingUnit& task)
    {
        std::lock_guard _{task_queue_mutex_};

        task_queue_.push_back(task);

        std::sort(
            DAS_FULL_RANGE_OF(task_queue_),
            [](const SchedulingUnit& lhs, const SchedulingUnit& rhs)
            { return lhs.utc_next_run_time <= rhs.utc_next_run_time; });
    }

    void TaskScheduler::SetErrorMessage(const std::string& message)
    {
        DasResult create_error_message_result;
        last_task_execute_message_ =
            DasReadOnlyString::FromUtf8(message, &create_error_message_result);
        if (IsFailed(create_error_message_result))
        {
            DAS_CORE_LOG_ERROR(
                "Can not save error message. Error code = {}",
                create_error_message_result);
            return;
        }
    }

    void TaskScheduler::DoTask(const SchedulingUnit& schedule_unit)
    {
        DAS_CORE_LOG_INFO("Enter!");
        bool       is_success{false};
        DasResult  do_error_code{DAS_E_UNDEFINED_RETURN_VALUE};
        const auto p_task = schedule_unit.p_task_info->GetTask();
        last_task_ = {p_task};
        try
        {
            const auto task_name = Utils::GetRuntimeClassNameFrom(p_task);
            const auto guid = Utils::GetGuidFrom(p_task);
            DAS_CORE_LOG_ERROR(
                "Begin run task. Name = {}, guid = {}, code = {}.",
                task_name,
                guid,
                do_error_code);
            DasPtr<IDasReadOnlyString> p_environment_config{};
            environment_config_.GetValue(p_environment_config.Put());
            DAS_CORE_LOG_INFO("Dump env config:\n{}", p_environment_config);
            DasPtr<IDasReadOnlyString> p_plugin_config{};
            DasPtr                     p_settings_json =
                schedule_unit.p_task_info->GetSettingsJson();
            const auto p_stop_token = task_controller_.ExecuteAtomically(
                [](TaskController& self)
                { return static_cast<IDasStopToken*>(self.stop_token_); });
            const auto do_result = p_task->Do(
                p_stop_token,
                p_environment_config.Get(),
                p_settings_json.Get());
            do_error_code = GetErrorCodeFrom(do_result);
            if (IsOk(do_result))
            {
                is_success = true;
                const auto message = DAS::fmt::format(
                    "Task execution success. Name = {}, guid = {}, code = {}.",
                    task_name,
                    guid,
                    do_error_code);
                DAS_CORE_LOG_ERROR(message);
                SetErrorMessage(message);
                return;
            }

            // failed
            const auto error_message = DAS::fmt::format(
                "Task execution failed. Name = {}, guid = {}, code = {}.",
                task_name,
                guid,
                do_error_code);
            DAS_CORE_LOG_ERROR(error_message);

            DasReadOnlyString          task_error_message{};
            DasPtr<IDasReadOnlyString> p_task_error_message{};
            const auto create_task_error_message_result = ::DasGetErrorMessage(
                p_task,
                do_error_code,
                p_task_error_message.Put());
            if (IsFailed(create_task_error_message_result))
            {
                DAS_CORE_LOG_ERROR(
                    "Get task error message failed. Error code = {}.",
                    create_task_error_message_result);
                SetErrorMessage(error_message);
                return;
            }
            task_error_message = p_task_error_message;

            const auto full_error_message = DAS::fmt::format(
                "{}\nMessage from task = \"{}\"",
                error_message,
                task_error_message.GetUtf8());
            SetErrorMessage(full_error_message);
        }
        catch (const DasException& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            std::string message;
            if (is_success)
            {
                message = DAS::fmt::format(
                    "Task execution success. Code = {}.",
                    do_error_code);
            }
            else
            {
                message = DAS::fmt::format(
                    "Task execution failed. Error code = {}.",
                    do_error_code);
            }

            DAS_CORE_LOG_ERROR(message);
            SetErrorMessage(message);
        }
        catch (const std::runtime_error& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            if (do_error_code == DAS_E_UNDEFINED_RETURN_VALUE)
            {
                do_error_code = DAS_E_INTERNAL_FATAL_ERROR;
            }
            SetErrorMessage(ex.what());
        }
    }

    void TaskScheduler::DumpStateToFile() {}

    DasResult TaskScheduler::AddTask(
        ForeignInterfaceHost::TaskManager::TaskInfo* p_task)
    {
        DAS_CORE_LOG_INFO("Enter!");
        try
        {
            // 构造一个时间出来
            SchedulingUnit scheduling_unit{p_task};
            InternalAddTask(scheduling_unit);
        }
        catch (const DasException& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            const std::string message = DAS::fmt::format(
                "Get next execution time success. Error code = {}.",
                ex.GetErrorCode());
            DAS_CORE_LOG_ERROR(message);
            return ex.GetErrorCode();
        }
        catch (const std::runtime_error& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            return DAS_E_INTERNAL_FATAL_ERROR;
        }
        return DAS_S_OK;
    }

    void TaskScheduler::RunTaskQueue()
    {
        if (task_controller_.ExecuteAtomically(
                [](TaskController& self)
                {
                    if (self.is_task_working_)
                    {
                        return true;
                    }
                    self.is_task_working_ = true;
                    self.stop_token_.Reset();
                    return false;
                }))
        {
            return;
        }

        std::optional<SchedulingUnit> opt_current_task{};
        {
            std::lock_guard _{task_queue_mutex_};
            if (task_queue_.empty())
            {
                task_controller_.ExecuteAtomically(
                    [](TaskController& self)
                    { self.is_task_working_ = false; });
                return;
            }
            const auto& current_task = task_queue_.back();
            if (current_task.utc_next_run_time
                > std::chrono::system_clock::to_time_t(
                    std::chrono::system_clock::now()))
            {
                task_controller_.ExecuteAtomically(
                    [](TaskController& self)
                    { self.is_task_working_ = false; });
                return;
            }
            opt_current_task = task_queue_.back();
            task_queue_.pop_back();
        }

        stdexec::start_on(
            GetSchedulerImpl(),
            stdexec::just()
                | stdexec::then(
                    [sp_this = DasPtr{this}, opt_current_task]
                    {
                        if (!opt_current_task)
                        {
                            DAS_CORE_LOG_ERROR(
                                "Empty task schedule unit found!");
                            return;
                        }
                        const auto& schedule_unit = opt_current_task.value();
                        sp_this->DoTask(schedule_unit);
                        sp_this->AddTask(schedule_unit.p_task_info.Get());
                        sp_this->task_controller_.ExecuteAtomically(
                            [](TaskController& self)
                            { self.is_task_working_ = false; });
                    }));
    }

    void TaskScheduler::NotifyExit()
    {
        is_not_need_exit_ = false;
        vm_thread_pool_.request_stop();
        executor_.detach();
    }

    void to_json(nlohmann::json& out, const TaskScheduler::SchedulingUnit& in)
    {
        DasGuid    iid;
        const auto get_iid_result = IsFailed(in.p_task_info->GetIid(&iid));
        if (IsFailed(get_iid_result))
        {
            DAS_CORE_LOG_ERROR("Failed to get iid.");
        }
        const auto guid = DAS::fmt::format("{}", iid);
        DasPtr     name = in.p_task_info->GetName();
        out = nlohmann::json{
            {"utcNextRunTime", in.utc_next_run_time},
            {"name", name},
            {"guid", guid.c_str()}};
    }

    DasResult TaskScheduler::GetAllTaskSchedulerInfo(
        IDasReadOnlyString** pp_out_json)
    {
        DasResult      error_code{DAS_E_UNDEFINED_RETURN_VALUE};
        nlohmann::json j{};
        try
        {
            {
                std::lock_guard _{task_queue_mutex_};
                j = {{"value", task_queue_}};
            }
            DasReadOnlyStringWrapper wrapper{j.dump()};
            wrapper.GetImpl(pp_out_json);
            error_code = DAS_S_OK;
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            error_code = DAS_E_INVALID_JSON;
        }
        catch (const std::bad_alloc& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            error_code = DAS_E_OUT_OF_MEMORY;
        }
        catch (const DasException& ex)
        {
            DAS_CORE_LOG_EXCEPTION(ex);
            error_code = ex.GetErrorCode();
            DAS_CORE_LOG_ERROR("Can not create task scheduler info.");
        }

        return error_code;
    }

    TaskScheduler::~TaskScheduler() { NotifyExit(); }

    DAS_DEFINE_VARIABLE(g_scheduler){};
} // namespace Core

DAS_NS_END

DasResult InitializeGlobalTaskScheduler()
{
    if (DAS::Core::g_scheduler)
    {
        DAS_CORE_LOG_ERROR("Global scheduler has been initialized.");
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
    DAS::Core::g_scheduler = DAS::MakeDasPtr<DAS::Core::TaskScheduler>();
    return DAS_S_OK;
}

DasResult GetIDasTaskScheduler(IDasTaskScheduler** pp_out_task_scheduler)
{
    DAS::Utils::SetResult(Das::Core::g_scheduler, pp_out_task_scheduler);
    return DAS_S_OK;
}
