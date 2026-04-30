#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

namespace Das::Core::TaskScheduler
{

    using Das::Core::ForeignInterfaceHost::FindManifest;
    using Das::Core::ForeignInterfaceHost::PluginPackageDesc;
    using Das::Core::ForeignInterfaceHost::PluginSettingDesc;
    using Das::Core::ForeignInterfaceHost::TaskDescriptor;

    namespace
    {
        using SystemClock = std::chrono::system_clock;

        // Sentinel far-future timestamp used when a task's
        // GetNextExecutionTime() fails. Prevents zero-delay hot loops by
        // pushing the next run far into the future.
        // 2099-12-31T23:59:59 UTC
        static constexpr int64_t SENTINEL_FUTURE_TIMESTAMP = 4102444799LL;

        time_t TimegmPortable(std::tm* tm)
        {
#ifdef _WIN32
            return _mkgmtime(tm);
#else
            return timegm(tm);
#endif
        }

        int64_t DasDateToUnix(const Das::ExportInterface::DasDate& date)
        {
            std::tm tm{};
            tm.tm_year = date.year - 1900;
            tm.tm_mon = date.month - 1;
            tm.tm_mday = date.day;
            tm.tm_hour = date.hour;
            tm.tm_min = date.minute;
            tm.tm_sec = date.second;
            tm.tm_isdst = -1;
            return static_cast<int64_t>(TimegmPortable(&tm));
        }

        std::chrono::steady_clock::duration ClampNextDelay(
            const std::chrono::seconds& delay)
        {
            if (delay <= std::chrono::seconds::zero())
            {
                return std::chrono::steady_clock::duration::zero();
            }
            return std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(delay);
        }
    } // namespace

    // ----------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------

    static std::string GuidToString(const DasGuid& guid)
    {
        return DAS_FMT_NS::format("{}", guid);
    }

    static std::chrono::steady_clock::duration ComputeNextDelay(
        const std::vector<TaskInstanceRecord>& task_instances,
        int64_t                                now_unix)
    {
        bool    has_future_due = false;
        int64_t earliest_due = 0;

        for (const auto& inst : task_instances)
        {
            if (inst.availability != TaskAvailability::Available
                || !inst.task_instance)
            {
                continue;
            }

            if (!inst.next_execution_time)
            {
                return std::chrono::steady_clock::duration::zero();
            }

            int64_t due = *inst.next_execution_time;

            if (due <= now_unix)
            {
                return std::chrono::steady_clock::duration::zero();
            }

            if (!has_future_due || due < earliest_due)
            {
                earliest_due = due;
                has_future_due = true;
            }
        }

        if (!has_future_due)
        {
            return std::chrono::seconds(1);
        }

        return ClampNextDelay(std::chrono::seconds(earliest_due - now_unix));
    }

    static TaskInstanceRecord* FindNextRunnableTask(
        std::vector<TaskInstanceRecord>& task_instances,
        int64_t                          now_unix)
    {
        TaskInstanceRecord* selected = nullptr;
        int64_t             selected_due = 0;
        bool                selected_has_due = false;

        for (auto& inst : task_instances)
        {
            if (inst.availability != TaskAvailability::Available
                || !inst.task_instance)
            {
                continue;
            }

            if (!inst.next_execution_time)
            {
                return &inst;
            }

            int64_t due = *inst.next_execution_time;

            if (due > now_unix)
            {
                continue;
            }

            if (!selected || !selected_has_due || due < selected_due)
            {
                selected = &inst;
                selected_due = due;
                selected_has_due = true;
            }
        }

        return selected;
    }

    TaskTypeRecord* SchedulerService::FindTaskType(const DasGuid& task_guid)
    {
        for (auto& ttr : task_types_)
        {
            if (ttr->task_guid == task_guid)
            {
                return ttr.get();
            }
        }
        return nullptr;
    }

    TaskInstanceRecord* SchedulerService::FindTaskInstance(int64_t task_id)
    {
        for (auto& inst : task_instances_)
        {
            if (inst.id == task_id)
            {
                return &inst;
            }
        }
        return nullptr;
    }

    const TaskInstanceRecord* SchedulerService::FindTaskInstance(
        int64_t task_id) const
    {
        for (const auto& inst : task_instances_)
        {
            if (inst.id == task_id)
            {
                return &inst;
            }
        }
        return nullptr;
    }

    // ----------------------------------------------------------------
    // Construction
    // ----------------------------------------------------------------

    SchedulerService::SchedulerService(
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
        Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context)
        : plugin_manager_(plugin_manager), ipc_context_{std::move(ipc_context)}
    {
        config_persist_thread_ =
            std::thread(&SchedulerService::ConfigPersistThreadLoop, this);
    }

    SchedulerService::~SchedulerService()
    {
        ShutdownConfigPersistQueue();
        if (disable_thread_.joinable())
        {
            disable_thread_.join();
        }
    }

    DasResult SchedulerService::CreateTaskInstance(
        const TaskTypeRecord&            task_type,
        Das::PluginInterface::IDasTask** pp_out_task)
    {
        if (!pp_out_task)
        {
            return DAS_E_INVALID_POINTER;
        }

        DasOutPtr<Das::PluginInterface::IDasTask> result(pp_out_task);

        auto create_result = plugin_manager_.CreateFeatureInterface(
            task_type.plugin_guid,
            task_type.feature_index,
            DasIidOf<Das::PluginInterface::IDasTask>(),
            reinterpret_cast<void**>(result.Put()));
        if (IsOk(create_result) && result)
        {
            result.Keep();
            return create_result;
        }

        if (create_result != DAS_E_NOT_FOUND || !task_type.prototype_task)
        {
            return create_result;
        }

        result.Set(task_type.prototype_task.Get());
        result.Keep();
        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // Initialize
    // ----------------------------------------------------------------

    DasResult SchedulerService::Initialize(
        const std::filesystem::path& plugin_dir,
        const std::vector<DasGuid>&  disabled_guids)
    {
        // Phase 1: Short lock for state validation and cleanup
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                DAS_CORE_LOG_ERROR(
                    "SchedulerService::Initialize: reject, scheduler is "
                    "Running or Stopping");
                return DAS_E_TASK_WORKING;
            }

            if (initialized_)
            {
                // Re-initialize: clear existing runtime state
                task_types_.clear();
                task_instances_.clear();
                for (auto it = loaded_plugin_paths_.rbegin();
                     it != loaded_plugin_paths_.rend();
                     ++it)
                {
                    plugin_manager_.UnloadPlugin(*it);
                }
                loaded_plugin_paths_.clear();
                initialized_ = false;
            }
        } // lock released

        // Phase 2: Lock-free I/O — directory traversal, manifest reading,
        // plugin loading, and SettingsManager reads all execute outside
        // mutex_.
        std::error_code ec;
        auto            dir_iter = std::filesystem::directory_iterator(
            plugin_dir,
            std::filesystem::directory_options::skip_permission_denied,
            ec);
        if (ec)
        {
            DAS_CORE_LOG_WARN(
                "SchedulerService::Initialize: failed to iterate "
                "plugin directory {}: {}",
                plugin_dir.string(),
                ec.message());
            return DAS_E_FAIL;
        }

        // Temp vectors to accumulate loaded data outside the lock
        std::vector<std::filesystem::path>           temp_paths;
        std::vector<std::unique_ptr<TaskTypeRecord>> temp_types;

        // Load allowed plugin packages
        for (const auto& entry : dir_iter)
        {
            std::filesystem::path manifest_path;
            if (entry.is_directory())
            {
                auto dirname = entry.path().filename().string();
                auto marker = entry.path() / (dirname + ".willBeDelete");
                if (std::filesystem::exists(marker))
                {
                    continue;
                }

                manifest_path = FindManifest(entry.path());
            }
            else if (entry.path().extension() == ".json")
            {
                manifest_path = entry.path();
            }

            if (manifest_path.empty())
            {
                continue;
            }

            try
            {
                std::ifstream     ifs(manifest_path);
                std::stringstream ss;
                ss << ifs.rdbuf();
                auto content = ss.str();
                auto json_data = Das::Utils::ParseYyjsonFromString(content);
                if (!json_data)
                {
                    DAS_CORE_LOG_WARN(
                        "SchedulerService::Initialize: failed to "
                        "parse manifest: {}",
                        manifest_path.string());
                    continue;
                }

                PluginPackageDesc desc;
                auto              obj = json_data->as_object();
                if (obj)
                {
                    const auto& jo = *obj;
                    // Parse name
                    auto name_val = jo[std::string_view("name")];
                    auto name_str = name_val.as_string();
                    if (name_str)
                    {
                        desc.name = std::string(*name_str);
                    }
                    // Parse guid
                    auto guid_val = jo[std::string_view("guid")];
                    auto guid_str = guid_val.as_string();
                    if (guid_str)
                    {
                        desc.guid =
                            Das::Core::ForeignInterfaceHost::MakeDasGuid(
                                std::string(*guid_str));
                    }
                }

                if (std::find(
                        disabled_guids.begin(),
                        disabled_guids.end(),
                        desc.guid)
                    != disabled_guids.end())
                {
                    DAS_CORE_LOG_INFO(
                        "SchedulerService::Initialize: skipping "
                        "disabled plugin: {}",
                        desc.name);
                    continue;
                }

                auto load_result = plugin_manager_.LoadPlugin(manifest_path);
                if (DAS::IsFailed(load_result))
                {
                    DAS_CORE_LOG_WARN(
                        "SchedulerService::Initialize: failed to "
                        "load plugin {}, result={}",
                        desc.name,
                        load_result);
                    continue;
                }

                auto register_result =
                    plugin_manager_.RegisterPluginObjects(manifest_path);
                if (DAS::IsFailed(register_result))
                {
                    DAS_CORE_LOG_WARN(
                        "SchedulerService::Initialize: failed to register "
                        "plugin objects for {}, result={}",
                        desc.name,
                        register_result);
                    plugin_manager_.UnloadPlugin(manifest_path);
                    continue;
                }

                temp_paths.push_back(manifest_path);
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::Initialize: failed to parse "
                    "manifest: {}",
                    e.what());
            }
        }

        // Collect IDasTask features and build available task types
        auto task_features = plugin_manager_.GetFeaturesByType(
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK);
        for (auto* feature_info : task_features)
        {
            DasPtr<Das::PluginInterface::IDasTask> task;
            auto qi_result = feature_info->interface_ptr->QueryInterface(
                DasIidOf<Das::PluginInterface::IDasTask>(),
                reinterpret_cast<void**>(task.Put()));
            if (!IsOk(qi_result) || !task)
            {
                continue;
            }

            auto type_record = std::make_unique<TaskTypeRecord>();
            type_record->prototype_task = task;
            type_record->plugin_guid = feature_info->plugin_guid;
            type_record->feature_index = feature_info->feature_index;

            // Get task type GUID from IDasTypeInfo
            DasGuid task_guid{};
            auto    guid_result = task->GetGuid(&task_guid);
            if (!IsOk(guid_result))
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::Initialize: failed to get task "
                    "GUID, result={}",
                    guid_result);
                continue;
            }
            type_record->task_guid = task_guid;

            // Find manifest task descriptor metadata
            auto* pkg_desc = plugin_manager_.FindPluginPackageByGuid(
                feature_info->plugin_guid);
            if (pkg_desc)
            {
                auto td_it = pkg_desc->task_descriptors.find(task_guid);
                if (td_it != pkg_desc->task_descriptors.end())
                {
                    const auto& td = td_it->second;
                    type_record->name = td.name;
                    type_record->description = td.description;
                    type_record->game_name = td.game_name;
                    type_record->descriptors = td.descriptors;
                }
            }

            temp_types.push_back(std::move(type_record));
        }

        DAS_CORE_LOG_INFO(
            "SchedulerService::Initialize: discovered {} task types",
            temp_types.size());

        // Phase 3: SettingsManager reads for task instances (lock-free
        // from SchedulerService::mutex_ perspective; SettingsManager uses
        // per-key mutex internally).
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  scheduler_index = settings.GetSchedulerIndexJson("0");

        std::vector<TaskInstanceRecord> temp_instances;

        auto sched_obj = scheduler_index.as_object();
        if (sched_obj && sched_obj->contains(std::string_view("taskOrder"))
            && sched_obj->operator[](std::string_view("taskOrder")).is_array())
        {
            auto task_order_arr =
                *sched_obj->operator[](std::string_view("taskOrder"))
                     .as_array();
            for (auto arr_it = task_order_arr.begin();
                 arr_it != task_order_arr.end();
                 ++arr_it)
            {
                const auto& task_id_val = *arr_it;
                if (!task_id_val.is_int())
                {
                    continue;
                }
                int64_t tid = static_cast<int64_t>(task_id_val);

                auto instance_json = settings.GetTaskInstanceJson("0", tid);
                auto inst_obj_opt = instance_json.as_object();
                TaskInstanceRecord rec;
                rec.id = tid;

                if (!inst_obj_opt || !instance_json.is_object())
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason =
                        "task instance file is missing or corrupt";
                    temp_instances.push_back(std::move(rec));
                    continue;
                }
                const auto& inst_obj = *inst_obj_opt;

                // Parse taskGuid
                auto tg_val = inst_obj[std::string_view("taskGuid")];
                if (!tg_val.is_string())
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason = "invalid taskGuid";
                    temp_instances.push_back(std::move(rec));
                    continue;
                }
                {
                    auto tg_view = *tg_val.as_string();
                    rec.task_guid =
                        Das::Core::ForeignInterfaceHost::MakeDasGuid(
                            std::string(tg_view));
                }

                // Parse pluginGuid
                if (inst_obj.contains(std::string_view("pluginGuid")))
                {
                    auto pg_val = inst_obj[std::string_view("pluginGuid")];
                    if (pg_val.is_string())
                    {
                        auto pg_view = *pg_val.as_string();
                        rec.plugin_guid =
                            Das::Core::ForeignInterfaceHost::MakeDasGuid(
                                std::string(pg_view));
                    }
                }

                // Parse nextExecutionTime
                auto net_key = std::string_view("nextExecutionTime");
                if (inst_obj.contains(net_key) && !inst_obj[net_key].is_null())
                {
                    const auto& net_val = inst_obj[net_key];
                    auto        net_int = net_val.as_sint();
                    if (net_int)
                    {
                        rec.next_execution_time = *net_int;
                    }
                }

                // Parse properties
                auto props_key = std::string_view("properties");
                if (inst_obj.contains(props_key)
                    && inst_obj[props_key].is_object())
                {
                    auto props_serialized =
                        inst_obj[props_key].write(yyjson::WriteFlag::NoFlag);
                    auto props_parsed = Das::Utils::ParseYyjsonFromString(
                        std::string_view(
                            props_serialized.data(),
                            props_serialized.size()));
                    if (props_parsed)
                    {
                        rec.properties = std::move(*props_parsed);
                    }
                    else
                    {
                        rec.properties = Das::Utils::MakeYyjsonObject();
                    }
                }
                else
                {
                    rec.properties = Das::Utils::MakeYyjsonObject();
                }

                // Link to available task type — note: FindTaskType
                // searches temp_types (not yet committed), so we
                // search it manually here
                TaskTypeRecord* type_rec = nullptr;
                for (auto& ttr : temp_types)
                {
                    if (ttr->task_guid == rec.task_guid)
                    {
                        type_rec = ttr.get();
                        break;
                    }
                }

                if (type_rec)
                {
                    rec.task_type = type_rec;
                    rec.plugin_guid = type_rec->plugin_guid;
                    auto create_result =
                        CreateTaskInstance(*type_rec, rec.task_instance.Put());
                    if (IsOk(create_result) && rec.task_instance)
                    {
                        rec.availability = TaskAvailability::Available;
                    }
                    else
                    {
                        rec.availability = TaskAvailability::Unavailable;
                        rec.unavailability_reason =
                            "failed to create task instance";
                    }
                }
                else
                {
                    rec.availability = TaskAvailability::Unavailable;
                    rec.unavailability_reason =
                        "task type not found among loaded plugins";
                }

                temp_instances.push_back(std::move(rec));
            }
        }

        // Phase 4: Short lock to commit results to runtime state
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_types_ = std::move(temp_types);
            task_instances_ = std::move(temp_instances);
            loaded_plugin_paths_ = std::move(temp_paths);
            initialized_ = true;
        }

        DAS_CORE_LOG_INFO(
            "SchedulerService::Initialize: materialized {} task instances",
            task_instances_.size());

        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // Enable / Disable / Status
    // ----------------------------------------------------------------

    DasResult SchedulerService::Enable()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_.load() != SchedulerState::Stopped)
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Enable: invalid state, must be "
                "Stopped");
            return DAS_E_FAIL;
        }

        if (!initialized_)
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Enable: not initialized, call "
                "Initialize first");
            return DAS_E_OBJECT_NOT_INIT;
        }

        // Check for at least one available task instance
        bool has_available = false;
        for (const auto& inst : task_instances_)
        {
            if (inst.availability == TaskAvailability::Available)
            {
                has_available = true;
                break;
            }
        }
        if (!has_available)
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Enable: no available task instances");
            return DAS_E_OBJECT_NOT_INIT;
        }

        state_.store(SchedulerState::Running);

        if (!tick_timer_)
        {
            tick_timer_ = std::make_unique<boost::asio::steady_timer>(
                ipc_context_.get().GetIoContext());
        }

        int64_t now_unix = SystemClock::to_time_t(SystemClock::now());
        StartTickTimer(ComputeNextDelay(task_instances_, now_unix));

        DAS_CORE_LOG_INFO("SchedulerService::Enable: scheduler started");
        return DAS_S_OK;
    }

    DasResult SchedulerService::Disable()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (state_.load() == SchedulerState::Stopping)
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::Disable: already stopping, ignored");
                return DAS_S_OK;
            }

            if (state_.load() != SchedulerState::Running)
            {
                DAS_CORE_LOG_ERROR(
                    "SchedulerService::Disable: invalid state, must be "
                    "Running");
                return DAS_E_FAIL;
            }

            state_.store(SchedulerState::Stopping);

            // Cancel the timer but keep the object alive until the scheduler is
            // torn down, otherwise Boost.Asio may still hold internal
            // references to the timer entry while draining the io_context.
            if (tick_timer_)
            {
                tick_timer_->cancel();
            }

            // Request cooperative cancellation on the stop token.
            // stop_token_ is always created via DasStopTokenImpl::Make() in
            // OnTick(), so this static_cast is safe as long as OnTick remains
            // the sole creator.
            if (stop_token_)
            {
                auto* impl = static_cast<Das::Core::Utils::DasStopTokenImpl*>(
                    stop_token_.Get());
                if (impl)
                {
                    impl->RequestStop();
                }
            }
        }

        // Spawns a dedicated thread to wait for the current task to complete.
        // The Stopping state guard above guarantees only one such thread exists
        // at any time, so defensive joinable() check is unnecessary here.
        disable_thread_ = std::thread(
            [this]
            {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return current_task_ == nullptr; });
                }

                stop_token_.Reset();
                state_.store(SchedulerState::Stopped);
                DAS_CORE_LOG_INFO(
                    "SchedulerService::Disable: scheduler stopped");
            });

        return DAS_S_OK;
    }

    SchedulerState SchedulerService::Status() const { return state_.load(); }

    // ----------------------------------------------------------------
    // Get: merged scheduler state JSON
    // ----------------------------------------------------------------

    yyjson::writer::detail::value SchedulerService::Get()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto result = Das::Utils::MakeYyjsonObject();
        auto result_obj = *result.as_object();

        result_obj[std::string_view("state")] = std::string_view(
            state_.load() == SchedulerState::Running    ? "running"
            : state_.load() == SchedulerState::Stopping ? "stopping"
                                                        : "stopped");

        // Available task types
        auto types_arr = Das::Utils::MakeYyjsonArray();
        auto types_arr_ref = *types_arr.as_array();
        for (const auto& ttr : task_types_)
        {
            auto type_obj = Das::Utils::MakeYyjsonObject();
            auto type_ref = *type_obj.as_object();
            type_ref[std::string_view("taskGuid")] =
                yyjson::writer::detail::value(GuidToString(ttr->task_guid));
            type_ref[std::string_view("pluginGuid")] =
                yyjson::writer::detail::value(GuidToString(ttr->plugin_guid));
            type_ref[std::string_view("name")] =
                yyjson::writer::detail::value(ttr->name);
            type_ref[std::string_view("description")] =
                yyjson::writer::detail::value(ttr->description);
            if (ttr->game_name)
            {
                type_ref[std::string_view("gameName")] =
                    yyjson::writer::detail::value(*ttr->game_name);
            }

            // Include descriptors
            auto desc_arr = Das::Utils::MakeYyjsonArray();
            auto desc_arr_ref = *desc_arr.as_array();
            for (const auto& d : ttr->descriptors)
            {
                auto desc_obj = Das::Utils::MakeYyjsonObject();
                auto desc_ref = *desc_obj.as_object();
                desc_ref[std::string_view("name")] =
                    yyjson::writer::detail::value(d.name);
                desc_ref[std::string_view("type")] =
                    static_cast<int64_t>(d.type);
                desc_ref[std::string_view("required")] = d.required;
                if (d.description)
                {
                    desc_ref[std::string_view("description")] =
                        yyjson::writer::detail::value(*d.description);
                }
                desc_arr_ref.emplace_back(std::move(desc_obj));
            }
            type_ref[std::string_view("descriptors")] = std::move(desc_arr);
            types_arr_ref.emplace_back(std::move(type_obj));
        }
        result_obj[std::string_view("availableTaskTypes")] =
            std::move(types_arr);

        // Queued task instances
        auto tasks_arr = Das::Utils::MakeYyjsonArray();
        auto tasks_arr_ref = *tasks_arr.as_array();
        for (const auto& inst : task_instances_)
        {
            auto task_obj = Das::Utils::MakeYyjsonObject();
            auto task_ref = *task_obj.as_object();
            task_ref[std::string_view("id")] = inst.id;
            task_ref[std::string_view("taskGuid")] =
                yyjson::writer::detail::value(GuidToString(inst.task_guid));
            task_ref[std::string_view("pluginGuid")] =
                yyjson::writer::detail::value(GuidToString(inst.plugin_guid));

            if (inst.availability == TaskAvailability::Available)
            {
                task_ref[std::string_view("availability")] =
                    std::string_view("available");
            }
            else if (inst.availability == TaskAvailability::Unavailable)
            {
                task_ref[std::string_view("availability")] =
                    std::string_view("unavailable");
                task_ref[std::string_view("unavailabilityReason")] =
                    yyjson::writer::detail::value(inst.unavailability_reason);
            }
            else
            {
                task_ref[std::string_view("availability")] =
                    std::string_view("invalid");
                task_ref[std::string_view("unavailabilityReason")] =
                    yyjson::writer::detail::value(inst.unavailability_reason);
            }

            if (inst.next_execution_time)
            {
                task_ref[std::string_view("nextExecutionTime")] =
                    *inst.next_execution_time;
            }
            else
            {
                task_ref[std::string_view("nextExecutionTime")] = nullptr;
            }

            // Serialize properties and re-parse to create an owning copy
            if (!inst.properties.is_null())
            {
                auto props_serialized =
                    inst.properties.write(yyjson::WriteFlag::NoFlag);
                auto props_parsed = Das::Utils::ParseYyjsonFromString(
                    std::string_view(
                        props_serialized.data(),
                        props_serialized.size()));
                if (props_parsed)
                {
                    task_ref[std::string_view("properties")] =
                        std::move(*props_parsed);
                }
                else
                {
                    task_ref[std::string_view("properties")] =
                        Das::Utils::MakeYyjsonObject();
                }
            }
            else
            {
                task_ref[std::string_view("properties")] =
                    Das::Utils::MakeYyjsonObject();
            }
            tasks_arr_ref.emplace_back(std::move(task_obj));
        }

        result_obj[std::string_view("tasks")] = std::move(tasks_arr);
        return result;
    }

    // ----------------------------------------------------------------
    // AddTask
    // ----------------------------------------------------------------

    DasResult SchedulerService::AddTask(
        const DasGuid& task_guid,
        int64_t*       out_task_id)
    {
        if (out_task_id == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        *out_task_id = 0;

        // Phase 1: Short lock for validation — copy type data to avoid
        // dangling pointer if Initialize/Disable clears task_types_.
        TaskTypeRecord type_snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return DAS_E_TASK_WORKING;
            }

            if (!initialized_)
            {
                return DAS_E_OBJECT_NOT_INIT;
            }

            auto* type_rec = FindTaskType(task_guid);
            if (!type_rec)
            {
                return DAS_E_NOT_FOUND;
            }

            type_snapshot = *type_rec;
        } // lock released

        // Phase 2: Lock-free persistence — all SettingsManager calls
        // and file I/O execute outside mutex_. Uses type_snapshot
        // (value copy) so no dangling pointer risk.
        auto&   settings = plugin_manager_.GetSettingsManager();
        auto    scheduler_index = settings.GetSchedulerIndexJson("0");
        int64_t next_id = 0;
        {
            auto si_obj = scheduler_index.as_object();
            if (si_obj)
            {
                auto nid_val =
                    si_obj->operator[](std::string_view("nextTaskId"));
                auto nid = nid_val.as_sint();
                if (nid)
                {
                    next_id = *nid;
                }
            }
        }

        // Build instance with descriptor defaults
        TaskInstanceRecord rec;
        rec.id = next_id;
        rec.task_guid = task_guid;
        rec.plugin_guid = type_snapshot.plugin_guid;
        rec.availability = TaskAvailability::Available;
        rec.properties = Das::Utils::MakeYyjsonObject();
        auto create_result =
            CreateTaskInstance(type_snapshot, rec.task_instance.Put());
        if (IsFailed(create_result) || !rec.task_instance)
        {
            return IsFailed(create_result) ? create_result : DAS_E_FAIL;
        }

        // Initialize properties from descriptor defaults
        {
            auto props_obj = rec.properties.as_object();
            for (const auto& desc : type_snapshot.descriptors)
            {
                if (!std::holds_alternative<std::monostate>(desc.default_value))
                {
                    std::visit(
                        [&props_obj, &desc](const auto& val)
                        {
                            if constexpr (!std::is_same_v<
                                              std::decay_t<decltype(val)>,
                                              std::monostate>)
                            {
                                props_obj->operator[](
                                    std::string_view(desc.name)) = val;
                            }
                        },
                        desc.default_value);
                }
            }
        }

        // Persist task instance file
        auto instance_json = Das::Utils::MakeYyjsonObject();
        auto inst_obj = *instance_json.as_object();
        inst_obj[std::string_view("id")] = rec.id;
        inst_obj[std::string_view("taskGuid")] =
            yyjson::writer::detail::value(GuidToString(rec.task_guid));
        inst_obj[std::string_view("pluginGuid")] =
            yyjson::writer::detail::value(GuidToString(rec.plugin_guid));
        inst_obj[std::string_view("nextExecutionTime")] = nullptr;

        // Serialize properties and re-parse to create an owning copy
        if (!rec.properties.is_null())
        {
            auto props_serialized =
                rec.properties.write(yyjson::WriteFlag::NoFlag);
            auto props_parsed = Das::Utils::ParseYyjsonFromString(
                std::string_view(
                    props_serialized.data(),
                    props_serialized.size()));
            if (props_parsed)
            {
                inst_obj[std::string_view("properties")] =
                    std::move(*props_parsed);
            }
            else
            {
                inst_obj[std::string_view("properties")] =
                    Das::Utils::MakeYyjsonObject();
            }
        }
        else
        {
            inst_obj[std::string_view("properties")] =
                Das::Utils::MakeYyjsonObject();
        }

        auto persist_result =
            settings.UpdateTaskInstanceJson("0", rec.id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Update scheduler index
        {
            auto si_obj = scheduler_index.as_object();
            if (si_obj)
            {
                si_obj->operator[](std::string_view("nextTaskId")) =
                    next_id + 1;
                auto order_arr =
                    si_obj->operator[](std::string_view("taskOrder"))
                        .as_array();
                if (order_arr)
                {
                    order_arr->emplace_back(next_id);
                }
            }
        }
        auto index_result =
            settings.UpdateSchedulerIndexJson("0", scheduler_index);
        if (IsFailed(index_result))
        {
            // Rollback task instance file (best-effort)
            settings.DeleteTaskInstanceJson("0", rec.id);
            return index_result;
        }

        // Phase 3: Short lock to commit to runtime state
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto*                       type_rec = FindTaskType(task_guid);
            rec.task_type = type_rec;
            task_instances_.push_back(std::move(rec));
        }

        *out_task_id = next_id;
        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // DeleteTask
    // ----------------------------------------------------------------

    DasResult SchedulerService::DeleteTask(int64_t task_id)
    {
        // Phase 1: Short lock to validate state and check existence
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return DAS_E_TASK_WORKING;
            }

            auto it = std::find_if(
                task_instances_.begin(),
                task_instances_.end(),
                [task_id](const TaskInstanceRecord& r)
                { return r.id == task_id; });

            if (it == task_instances_.end())
            {
                return DAS_E_NOT_FOUND;
            }
        } // lock released — Config side validation done

        // Phase 2: Lock-free SettingsManager persistence with retry.
        // All file I/O and sleep_for execute outside mutex_.
        auto& settings = plugin_manager_.GetSettingsManager();

        auto scheduler_index = settings.GetSchedulerIndexJson("0");
        {
            auto si_obj = scheduler_index.as_object();
            if (si_obj && si_obj->contains(std::string_view("taskOrder"))
                && si_obj->operator[](std::string_view("taskOrder")).is_array())
            {
                auto order_arr =
                    *si_obj->operator[](std::string_view("taskOrder"))
                         .as_array();
                // Build new array without the deleted task
                auto new_arr = Das::Utils::MakeYyjsonArray();
                auto new_arr_ref = *new_arr.as_array();
                for (auto oit = order_arr.begin(); oit != order_arr.end();
                     ++oit)
                {
                    if (!(*oit).is_int()
                        || static_cast<int64_t>(*oit) != task_id)
                    {
                        // Create owning copy of the value
                        auto serialized =
                            (*oit).write(yyjson::WriteFlag::NoFlag);
                        auto parsed = Das::Utils::ParseYyjsonFromString(
                            std::string_view(
                                serialized.data(),
                                serialized.size()));
                        if (parsed)
                        {
                            new_arr_ref.emplace_back(std::move(*parsed));
                        }
                    }
                }
                si_obj->operator[](std::string_view("taskOrder")) =
                    std::move(new_arr);
            }
        }

        constexpr int MAX_RETRIES = 3;
        constexpr int BASE_DELAY_MS = 100;
        DasResult     last_error = DAS_S_OK;

        for (int attempt = 0; attempt < MAX_RETRIES; ++attempt)
        {
            if (attempt > 0)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(BASE_DELAY_MS << (attempt - 1)));
            }

            auto delete_result = settings.DeleteTaskInstanceJson("0", task_id);
            auto index_result =
                settings.UpdateSchedulerIndexJson("0", scheduler_index);

            if (!DAS::IsFailed(delete_result) && !DAS::IsFailed(index_result))
            {
                // Persistence succeeded — remove from runtime state
                std::lock_guard<std::mutex> commit_lock(mutex_);
                auto                        it = std::find_if(
                    task_instances_.begin(),
                    task_instances_.end(),
                    [task_id](const TaskInstanceRecord& r)
                    { return r.id == task_id; });
                if (it != task_instances_.end())
                {
                    task_instances_.erase(it);
                }
                return DAS_S_OK;
            }

            last_error =
                DAS::IsFailed(delete_result) ? delete_result : index_result;
        }

        // All retries failed: runtime state unchanged (task remains in
        // task_instances_), no rollback needed.
        DAS_CORE_LOG_ERROR(
            "SchedulerService::DeleteTask: persistence failed for task {} "
            "after {} retries, result={}",
            task_id,
            MAX_RETRIES,
            last_error);

        return last_error;
    }

    // ----------------------------------------------------------------
    // UpdateTaskProperties
    // ----------------------------------------------------------------

    DasResult SchedulerService::UpdateTaskProperties(
        int64_t                              task_id,
        const yyjson::writer::detail::value& properties)
    {
        // Phase 1: Short lock for validation and snapshot — copy
        // descriptors to avoid dangling pointer across lock-free phases.
        std::vector<Das::Core::ForeignInterfaceHost::PluginSettingDesc>
                                      descriptors;
        yyjson::writer::detail::value current_properties;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return DAS_E_TASK_WORKING;
            }

            auto* inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return DAS_E_NOT_FOUND;
            }

            // Invalid/unavailable instances cannot have properties updated
            if (inst->availability != TaskAvailability::Available)
            {
                return DAS_E_FAIL;
            }

            if (!inst->task_type)
            {
                return DAS_E_FAIL;
            }

            descriptors = inst->task_type->descriptors;
            // Deep copy properties
            if (!inst->properties.is_null())
            {
                auto serialized =
                    inst->properties.write(yyjson::WriteFlag::NoFlag);
                auto parsed = Das::Utils::ParseYyjsonFromString(
                    std::string_view(serialized.data(), serialized.size()));
                if (parsed)
                {
                    current_properties = std::move(*parsed);
                }
                else
                {
                    current_properties = Das::Utils::MakeYyjsonObject();
                }
            }
            else
            {
                current_properties = Das::Utils::MakeYyjsonObject();
            }
        } // lock released

        if (!properties.is_object())
        {
            return DAS_E_INVALID_JSON;
        }

        // Validate property names and types against descriptors
        auto props_obj = properties.as_object();
        for (auto it = props_obj->begin(); it != props_obj->end(); ++it)
        {
            const auto& prop_name = it->first;
            const auto& prop_val = it->second;
            bool        found = false;
            for (const auto& desc : descriptors)
            {
                if (desc.name == std::string_view(prop_name))
                {
                    // Type validation
                    switch (desc.type)
                    {
                    case Das::ExportInterface::DAS_TYPE_BOOL:
                        if (!prop_val.is_bool())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_INT:
                        if (!prop_val.is_int())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_FLOAT:
                        if (!prop_val.is_num())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_STRING:
                        if (!prop_val.is_string())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    default:
                        break;
                    }
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return DAS_E_NOT_FOUND;
            }
        }

        // Apply validated properties to snapshot (mutable copy)
        {
            auto cur_obj = current_properties.as_object();
            for (auto it = props_obj->begin(); it != props_obj->end(); ++it)
            {
                const auto& prop_name = it->first;
                const auto& prop_val = it->second;
                // Deep copy the property value
                auto serialized = prop_val.write(yyjson::WriteFlag::NoFlag);
                auto parsed = Das::Utils::ParseYyjsonFromString(
                    std::string_view(serialized.data(), serialized.size()));
                if (parsed)
                {
                    cur_obj->operator[](std::string_view(prop_name)) =
                        std::move(*parsed);
                }
            }
        }

        // Phase 2: Lock-free SettingsManager persistence
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  instance_json = settings.GetTaskInstanceJson("0", task_id);
        if (instance_json.is_null() || !instance_json.is_object())
        {
            instance_json = Das::Utils::MakeYyjsonObject();
        }
        {
            auto inst_obj = instance_json.as_object();
            // Serialize current_properties and re-parse for owning copy
            auto props_serialized =
                current_properties.write(yyjson::WriteFlag::NoFlag);
            auto props_parsed = Das::Utils::ParseYyjsonFromString(
                std::string_view(
                    props_serialized.data(),
                    props_serialized.size()));
            if (props_parsed)
            {
                inst_obj->operator[](std::string_view("properties")) =
                    std::move(*props_parsed);
            }
        }
        auto persist_result =
            settings.UpdateTaskInstanceJson("0", task_id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Phase 3: Short lock to commit properties to runtime state
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto*                       inst = FindTaskInstance(task_id);
            if (inst)
            {
                inst->properties = std::move(current_properties);
            }
        }

        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // UpdateTaskInternalProperties
    // ----------------------------------------------------------------

    DasResult SchedulerService::UpdateTaskInternalProperties(
        int64_t                              task_id,
        const yyjson::writer::detail::value& internal_props)
    {
        // Phase 1: Short lock for validation and snapshot
        std::optional<int64_t> new_next_time;
        bool                   has_update = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (state_.load() == SchedulerState::Running
                || state_.load() == SchedulerState::Stopping)
            {
                return DAS_E_TASK_WORKING;
            }

            auto* inst = FindTaskInstance(task_id);
            if (!inst)
            {
                return DAS_E_NOT_FOUND;
            }

            // Parse nextExecutionTime if provided
            auto ip_obj = internal_props.as_object();
            auto net_key = std::string_view("nextExecutionTime");
            if (ip_obj && ip_obj->contains(net_key))
            {
                const auto& val = ip_obj->operator[](net_key);
                if (val.is_null())
                {
                    new_next_time = std::nullopt;
                    has_update = true;
                }
                else if (val.is_int())
                {
                    auto net_int = val.as_sint();
                    if (net_int)
                    {
                        new_next_time = *net_int;
                        has_update = true;
                    }
                    else
                    {
                        return DAS_E_TYPE_ERROR;
                    }
                }
                else
                {
                    return DAS_E_TYPE_ERROR;
                }
            }
        } // lock released

        if (!has_update)
        {
            return DAS_S_OK;
        }

        // Phase 2: Lock-free SettingsManager persistence
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  instance_json = settings.GetTaskInstanceJson("0", task_id);
        if (instance_json.is_null() || !instance_json.is_object())
        {
            instance_json = Das::Utils::MakeYyjsonObject();
        }
        {
            auto inst_obj = instance_json.as_object();
            if (new_next_time.has_value())
            {
                inst_obj->operator[](std::string_view("nextExecutionTime")) =
                    *new_next_time;
            }
            else
            {
                inst_obj->operator[](std::string_view("nextExecutionTime")) =
                    nullptr;
            }
        }
        auto persist_result =
            settings.UpdateTaskInstanceJson("0", task_id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Phase 3: Short lock to commit to runtime state
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto*                       inst = FindTaskInstance(task_id);
            if (inst)
            {
                inst->next_execution_time = new_next_time;
            }
        }

        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // Timer and tick
    // ----------------------------------------------------------------

    void SchedulerService::StartTickTimer(
        std::chrono::steady_clock::duration delay)
    {
        if (!tick_timer_)
        {
            return;
        }

        tick_timer_->expires_after(delay);
        tick_timer_->async_wait(
            [this](const boost::system::error_code& ec)
            {
                if (!ec && state_.load() == SchedulerState::Running)
                {
                    auto* callback = new TickCallback(this);
                    ipc_context_.get().PostToBusinessThread(callback);
                    callback->Release();
                }
            });
    }

    void SchedulerService::OnTick()
    {
        if (state_.load() != SchedulerState::Running)
        {
            return;
        }

        // Select a runnable task instance under lock, then release lock
        // before calling plugin Do.
        Das::PluginInterface::IDasTask*     task_ptr = nullptr;
        TaskInstanceRecord*                 selected_inst = nullptr;
        yyjson::writer::detail::value       properties_copy;
        int64_t                             selected_id = -1;
        std::chrono::steady_clock::duration next_delay =
            std::chrono::seconds(1);

        int64_t now_unix = SystemClock::to_time_t(SystemClock::now());

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (state_.load() != SchedulerState::Running)
            {
                return;
            }

            selected_inst = FindNextRunnableTask(task_instances_, now_unix);

            if (!selected_inst)
            {
                DAS_CORE_LOG_DEBUG(
                    "SchedulerService::OnTick: no runnable task");
                next_delay = ComputeNextDelay(task_instances_, now_unix);
                StartTickTimer(next_delay);
                return;
            }

            task_ptr = selected_inst->task_instance.Get();
            properties_copy = selected_inst->properties;
            selected_id = selected_inst->id;

            current_task_ = task_ptr;

            // Create or reuse stop token for cooperative cancellation
            if (!stop_token_)
            {
                stop_token_ = Das::Core::Utils::DasStopTokenImpl::Make();
            }
        }

        // Create IDasJson inputs outside the lock (no serialization round-trip)
        DasPtr<Das::ExportInterface::IDasJson> p_env_json;
        auto env_cr = CreateEmptyDasJson(p_env_json.Put());
        if (IsFailed(env_cr))
        {
            p_env_json.Reset();
        }

        DasPtr<Das::ExportInterface::IDasJson> p_props_json;
        try
        {
            p_props_json = Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
                std::move(properties_copy));
        }
        catch (const std::bad_alloc&)
        {
            DAS_CORE_LOG_WARN(
                "Failed to allocate IDasJson for task properties");
        }

        // Call IDasTask::Do WITHOUT holding the mutex
        auto do_result = task_ptr->Do(
            stop_token_.Get(),
            p_env_json ? p_env_json.Get() : nullptr,
            p_props_json ? p_props_json.Get() : nullptr);

        if (IsFailed(do_result))
        {
            DAS_CORE_LOG_WARN(
                "SchedulerService::OnTick: task {} Do returned result={}",
                selected_id,
                do_result);
        }

        // Refresh nextExecutionTime from the task
        int64_t                       refreshed_time = 0;
        bool                          has_refreshed_time = false;
        Das::ExportInterface::DasDate next_date{};
        auto net_result = task_ptr->GetNextExecutionTime(&next_date);
        if (IsOk(net_result))
        {
            refreshed_time = DasDateToUnix(next_date);
            has_refreshed_time = true;
        }
        else
        {
            // GetNextExecutionTime failed: use sentinel to prevent hot loop
            refreshed_time = SENTINEL_FUTURE_TIMESTAMP;
            has_refreshed_time = true;
            DAS_CORE_LOG_WARN(
                "SchedulerService::OnTick: task {} GetNextExecutionTime "
                "failed (result={}), setting sentinel nextExecutionTime={}",
                selected_id,
                net_result,
                SENTINEL_FUTURE_TIMESTAMP);
        }

        // Re-acquire lock to update in-memory state only.
        // Persistence is posted to config persist thread (no SettingsManager
        // call under mutex_).
        bool    should_persist = false;
        int64_t persist_id = 0;
        int64_t persist_time = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Update the instance record with refreshed nextExecutionTime
            if (selected_inst && has_refreshed_time)
            {
                selected_inst->next_execution_time = refreshed_time;
                should_persist = true;
                persist_id = selected_id;
                persist_time = refreshed_time;
            }

            current_task_ = nullptr;
            cv_.notify_all();

            if (state_.load() == SchedulerState::Running)
            {
                now_unix = SystemClock::to_time_t(SystemClock::now());
                next_delay = ComputeNextDelay(task_instances_, now_unix);
            }
        }

        DAS_CORE_LOG_DEBUG("SchedulerService::OnTick: tick complete");
        if (state_.load() == SchedulerState::Running)
        {
            StartTickTimer(next_delay);
        }

        // Post persistence event to config-side thread (outside mutex_).
        // Failures are logged by the persist thread and do not affect
        // runtime state.
        if (should_persist)
        {
            PostPersistEvent(persist_id, persist_time);
        }

        // Notify WebSocket clients of state change (server-triggered only)
        if (state_notify_)
        {
            auto state_json = Get();
            auto state_serialized =
                Das::Utils::SerializeYyjsonValue(state_json, false);
            if (state_serialized)
            {
                state_notify_(state_serialized->c_str());
            }
        }
    }

    // ----------------------------------------------------------------
    // Config-side persistence thread
    // ----------------------------------------------------------------

    void SchedulerService::SetStateNotifyCallback(
        SchedulerNotifyFunc func,
        void*               user_data)
    {
        state_notify_ = {func, user_data};
    }

    void SchedulerService::PostPersistEvent(int64_t task_id, int64_t next_time)
    {
        {
            std::lock_guard<std::mutex> lock(config_persist_mutex_);
            config_persist_queue_.push({task_id, next_time});
        }
        config_persist_cv_.notify_one();
    }

    void SchedulerService::ConfigPersistThreadLoop()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(config_persist_mutex_);
            config_persist_cv_.wait(
                lock,
                [this]
                {
                    return !config_persist_queue_.empty()
                           || config_persist_shutdown_.load();
                });

            // Drain all pending events before checking shutdown
            while (!config_persist_queue_.empty())
            {
                auto event = std::move(config_persist_queue_.front());
                config_persist_queue_.pop();
                lock.unlock();

                // Execute SettingsManager I/O without holding any
                // SchedulerService::mutex_
                auto& settings = plugin_manager_.GetSettingsManager();
                auto  instance_json =
                    settings.GetTaskInstanceJson("0", event.task_id);
                if (instance_json.is_null() || !instance_json.is_object())
                {
                    instance_json = Das::Utils::MakeYyjsonObject();
                }
                {
                    auto inst_obj = instance_json.as_object();
                    inst_obj->operator[](std::string_view(
                        "nextExecutionTime")) = event.next_execution_time;
                }
                auto result = settings.UpdateTaskInstanceJson(
                    "0",
                    event.task_id,
                    instance_json);

                if (DAS::IsFailed(result))
                {
                    DAS_CORE_LOG_ERROR(
                        "SchedulerService::ConfigPersistThread: failed to "
                        "persist nextExecutionTime for task {}, result={}",
                        event.task_id,
                        result);
                }

                lock.lock();
            }

            if (config_persist_shutdown_.load())
            {
                return;
            }
        }
    }

    void SchedulerService::ShutdownConfigPersistQueue()
    {
        config_persist_shutdown_.store(true);
        config_persist_cv_.notify_all();
        if (config_persist_thread_.joinable())
        {
            config_persist_thread_.join();
        }
    }

} // namespace Das::Core::TaskScheduler
