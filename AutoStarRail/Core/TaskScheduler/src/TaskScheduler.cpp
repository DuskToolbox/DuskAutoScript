#include <AutoStarRail/Core/ForeignInterfaceHost/AsrGuid.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/AsrStringImpl.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/Core/TaskScheduler/TaskScheduler.h>
#include <AutoStarRail/Core/Utils/InternalUtils.h>
#include <AutoStarRail/Core/Utils/StdExecution.h>

ASR_NS_BEGIN

namespace Core
{
    ASR_NS_ANONYMOUS_DETAILS_BEGIN

    time_t ToUTC(std::tm& time_info) noexcept
    {
#ifdef _WIN32
        std::time_t tt = _mkgmtime(&time_info);
#else
        time_t tt = timegm(&timeinfo);
#endif
        return tt;
    }

    auto CreateDateTime(const AsrDate& date) noexcept
    {
        tm time_info1 = tm();
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

    ASR_NS_ANONYMOUS_DETAILS_END

    bool TaskScheduler::SchedulingUnit::operator==(
        const SchedulingUnit& rhs) const noexcept
    {
        AsrGuid iid_lhs;
        AsrGuid iid_rhs;
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
        AsrGuid guid;
        p_task_info->GetIid(&guid);
        AsrDate    date{};
        const auto get_date_error_code =
            p_task_info->GetTask()->GetNextExecutionTime(&date);
        if (IsFailed(get_date_error_code))
        {
            AsrPtr p_name{p_task_info->GetName()};
            ASR_CORE_LOG_ERROR(
                "Can not get next execution time. Task name = {}, guid = {}, error code = {}",
                p_name,
                guid,
                get_date_error_code);
            AsrException::Throw(get_date_error_code);
        }
        utc_next_run_time = Details::CreateDateTime(date);
    }

    void TaskScheduler::EnvironmentConfig::SetValue(
        IAsrReadOnlyString* p_config)
    {
        std::lock_guard _{mutex_};
        environment_config_ = p_config;
    }

    void TaskScheduler::EnvironmentConfig::GetValue(
        IAsrReadOnlyString** pp_out_config)
    {
        std::lock_guard _{mutex_};
        Utils::SetResult(environment_config_, pp_out_config);
    }

    TaskScheduler::TaskScheduler()
        : executor_{[sp_this = AsrPtr{this}]
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

    AsrResult TaskScheduler::QueryInterface(
        const AsrGuid& iid,
        void**         pp_object)
    {
        return Utils::QueryInterface<IAsrTaskScheduler>(this, iid, pp_object);
    }

    ASR_NS_ANONYMOUS_DETAILS_BEGIN

    class IAsrTaskInfoVectorImpl final : public IAsrTaskInfoVector
    {
    public:
        using TaskVector = std::vector<TaskScheduler::SchedulingUnit>;
        IAsrTaskInfoVectorImpl(const TaskVector& tasks) : all_tasks{tasks} {}

    private:
        // IAsrBase
        ASR_UTILS_IASRBASE_AUTO_IMPL(IAsrTaskInfoVectorImpl)
        ASR_IMPL QueryInterface(const AsrGuid& iid, void** pp_object) override
        {
            return Utils::QueryInterface<IAsrTaskInfoVector>(
                this,
                iid,
                pp_object);
        }
        // IAsrTaskInfoVector
        ASR_IMPL EnumByIndex(size_t index, IAsrTaskInfo** pp_out_info) override
        {
            ASR_UTILS_CHECK_POINTER(pp_out_info);
            if (index > all_tasks.size())
            {
                return ASR_E_OUT_OF_RANGE;
            }
            Utils::SetResult(all_tasks[index].p_task_info, pp_out_info);
            return ASR_S_OK;
        }
        AsrResult EnumNextExecuteTimeByIndex(size_t index, time_t* p_out_time)
            override
        {
            ASR_UTILS_CHECK_POINTER(p_out_time);
            if (index > all_tasks.size())
            {
                return ASR_E_OUT_OF_RANGE;
            }
            *p_out_time = all_tasks[index].utc_next_run_time;
            return ASR_S_OK;
        }

        TaskVector all_tasks;
    };

    ASR_NS_ANONYMOUS_DETAILS_END

    AsrResult TaskScheduler::GetAllWorkingTasks(
        IAsrTaskInfoVector** pp_out_task_info_vector)
    {
        AsrPtr<Details::IAsrTaskInfoVectorImpl> p_result;
        {
            std::lock_guard _{task_queue_mutex_};
            p_result = MakeAsrPtr<Details::IAsrTaskInfoVectorImpl>(task_queue_);
        }
        Utils::SetResult(p_result, pp_out_task_info_vector);
        return ASR_S_OK;
    }

    AsrResult TaskScheduler::AddTask(IAsrTaskInfo* p_task_info)
    {
        AsrPtr<ForeignInterfaceHost::TaskManager::TaskInfo> p_task_info_impl{};
        if (const auto qi_result = p_task_info->QueryInterface(
                AsrIidOf<ForeignInterfaceHost::TaskManager::TaskInfo>(),
                p_task_info_impl.PutVoid());
            IsFailed(qi_result))
        {
            ASR_CORE_LOG_ERROR(
                "Can not find class ForeignInterfaceHost::TaskManager::TaskInfo.");
            return qi_result;
        }
        return AddTask(p_task_info_impl.Get());
    }

    AsrResult TaskScheduler::RemoveTask(IAsrTaskInfo* p_task_info)
    {
        AsrPtr<ForeignInterfaceHost::TaskManager::TaskInfo> p_task_info_impl{};
        if (const auto qi_result = p_task_info->QueryInterface(
                AsrIidOf<ForeignInterfaceHost::TaskManager::TaskInfo>(),
                p_task_info_impl.PutVoid());
            IsFailed(qi_result))
        {
            ASR_CORE_LOG_ERROR(
                "Can not find class ForeignInterfaceHost::TaskManager::TaskInfo.");
            return qi_result;
        }
        AsrGuid target_iid;
        p_task_info->GetIid(&target_iid);
        std::lock_guard _{task_queue_mutex_};
        const auto      it_to_be_removed = std::find_if(
            ASR_FULL_RANGE_OF(task_queue_),
            [target_iid](const SchedulingUnit& v)
            {
                AsrGuid v_iid;
                v.p_task_info->GetIid(&v_iid);
                return v_iid == target_iid;
            });
        if (it_to_be_removed == task_queue_.end())
        {
            return ASR_E_OUT_OF_RANGE;
        }
        task_queue_.erase(it_to_be_removed);
        return ASR_S_OK;
    }

    AsrResult TaskScheduler::UpdateEnvironmentConfig(
        IAsrReadOnlyString* p_config_json)
    {
        ASR_UTILS_CHECK_POINTER(p_config_json);

        environment_config_.SetValue(p_config_json);
        return ASR_S_OK;
    }

    void TaskScheduler::InternalAddTask(const SchedulingUnit& task)
    {
        std::lock_guard _{task_queue_mutex_};

        task_queue_.push_back(task);

        std::sort(
            ASR_FULL_RANGE_OF(task_queue_),
            [](const SchedulingUnit& lhs, const SchedulingUnit& rhs)
            { return lhs.utc_next_run_time <= rhs.utc_next_run_time; });
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

    void TaskScheduler::DoTask(const SchedulingUnit& schedule_unit)
    {
        ASR_CORE_LOG_INFO("Enter!");
        bool       is_success{false};
        AsrResult  do_error_code{ASR_E_UNDEFINED_RETURN_VALUE};
        const auto p_task = schedule_unit.p_task_info->GetTask();
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
            AsrPtr<IAsrReadOnlyString> p_environment_config{};
            environment_config_.GetValue(p_environment_config.Put());
            ASR_CORE_LOG_INFO("Dump env config:\n{}", p_environment_config);
            AsrPtr<IAsrReadOnlyString> p_plugin_config{};
            // taskinfo里面没有plugininfo的设置，看看怎么处理
            AsrPtr p_settings_json =
                schedule_unit.p_task_info->GetSettingsJson();
            const auto do_result =
                p_task->Do(p_environment_config.Get(), p_settings_json.Get());
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

            AsrReadOnlyString          task_error_message{};
            AsrPtr<IAsrReadOnlyString> p_task_error_message{};
            const auto create_task_error_message_result = ::AsrGetErrorMessage(
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

    AsrResult TaskScheduler::AddTask(
        ForeignInterfaceHost::TaskManager::TaskInfo* p_task)
    {
        ASR_CORE_LOG_INFO("Enter!");
        try
        {
            // 构造一个时间出来
            SchedulingUnit scheduling_unit{p_task};
            InternalAddTask(scheduling_unit);
        }
        catch (const AsrException& ex)
        {
            ASR_CORE_LOG_EXCEPTION(ex);
            const std::string message = ASR::fmt::format(
                "Get next execution time success. Error code = {}.",
                ex.GetErrorCode());
            ASR_CORE_LOG_ERROR(message);
            return ex.GetErrorCode();
        }
        catch (const std::runtime_error& ex)
        {
            ASR_CORE_LOG_EXCEPTION(ex);
            return ASR_E_INTERNAL_FATAL_ERROR;
        }
        return ASR_S_OK;
    }

    void TaskScheduler::RunTaskQueue()
    {
        if (is_task_working_)
        {
            return;
        }

        std::optional<SchedulingUnit> opt_current_task{};
        {
            std::lock_guard _{task_queue_mutex_};
            if (task_queue_.empty())
            {
                return;
            }
            opt_current_task = task_queue_.back();
            task_queue_.pop_back();
        }

        stdexec::start_on(
            GetSchedulerImpl(),
            stdexec::just()
                | stdexec::then(
                    [sp_this = AsrPtr{this}, opt_current_task]
                    {
                        sp_this->is_task_working_ = true;
                        if (!opt_current_task)
                        {
                            ASR_CORE_LOG_ERROR(
                                "Empty task schedule unit found!");
                            return;
                        }
                        const auto& schedule_unit = opt_current_task.value();
                        sp_this->DoTask(schedule_unit);
                        sp_this->AddTask(schedule_unit.p_task_info.Get());
                        sp_this->is_task_working_ = false;
                    }));
    }

    void TaskScheduler::NotifyExit()
    {
        is_not_need_exit_ = false;
        thread_pool.request_stop();
        executor_.detach();
    }

    void to_json(nlohmann::json& out, const TaskScheduler::SchedulingUnit& in)
    {
        AsrGuid    iid;
        const auto get_iid_result = IsFailed(in.p_task_info->GetIid(&iid));
        if (IsFailed(get_iid_result))
        {
            ASR_CORE_LOG_ERROR("Failed to get iid.");
        }
        const auto guid = ASR::fmt::format("{}", iid);
        AsrPtr     name = in.p_task_info->GetName();
        out = nlohmann::json{
            {"utcNextRunTime", in.utc_next_run_time},
            {"name", name},
            {"guid", guid.c_str()}};
    }

    AsrResult TaskScheduler::GetAllTaskSchedulerInfo(
        IAsrReadOnlyString** pp_out_json)
    {
        AsrResult      error_code{ASR_E_UNDEFINED_RETURN_VALUE};
        nlohmann::json j{};
        try
        {
            {
                std::lock_guard _{task_queue_mutex_};
                j = {{"value", task_queue_}};
            }
            AsrReadOnlyStringWrapper wrapper{j.dump()};
            wrapper.GetImpl(pp_out_json);
            error_code = ASR_S_OK;
        }
        catch (const nlohmann::json::exception& ex)
        {
            ASR_CORE_LOG_EXCEPTION(ex);
            error_code = ASR_E_INVALID_JSON;
        }
        catch (const std::bad_alloc& ex)
        {
            ASR_CORE_LOG_EXCEPTION(ex);
            error_code = ASR_E_OUT_OF_MEMORY;
        }
        catch (const AsrException& ex)
        {
            ASR_CORE_LOG_EXCEPTION(ex);
            error_code = ex.GetErrorCode();
            ASR_CORE_LOG_ERROR("Can not create task scheduler info.");
        }

        return error_code;
    }

    ASR_DEFINE_VARIABLE(g_scheduler);
} // namespace Core

ASR_NS_END