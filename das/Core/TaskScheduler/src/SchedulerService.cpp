#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/IDasBase.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Das::Core::TaskScheduler
{

    using Das::Core::ForeignInterfaceHost::FindManifest;
    using Das::Core::ForeignInterfaceHost::PluginPackageDesc;
    using Das::Core::ForeignInterfaceHost::PluginSettingDesc;
    using Das::Core::ForeignInterfaceHost::TaskDescriptor;

    // ----------------------------------------------------------------
    // Helpers
    // ----------------------------------------------------------------

    static std::string GuidToString(const DasGuid& guid)
    {
        char buf[64];
        std::snprintf(
            buf,
            sizeof(buf),
            "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            static_cast<unsigned long>(guid.data1),
            guid.data2,
            guid.data3,
            guid.data4[0],
            guid.data4[1],
            guid.data4[2],
            guid.data4[3],
            guid.data4[4],
            guid.data4[5],
            guid.data4[6],
            guid.data4[7]);
        return buf;
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
    }

    // ----------------------------------------------------------------
    // Initialize
    // ----------------------------------------------------------------

    DasResult SchedulerService::Initialize(
        const std::filesystem::path& plugin_dir,
        const std::vector<DasGuid>&  disabled_guids)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_.load() == SchedulerState::Running)
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Initialize: reject, scheduler is Running");
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

        if (!std::filesystem::exists(plugin_dir))
        {
            DAS_CORE_LOG_WARN(
                "SchedulerService::Initialize: plugin directory does "
                "not exist: {}",
                plugin_dir.string());
            return DAS_E_FAIL;
        }

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

        // Load allowed plugin packages
        for (const auto& entry : dir_iter)
        {
            if (!entry.is_directory())
            {
                continue;
            }

            auto dirname = entry.path().filename().string();
            auto marker = entry.path() / (dirname + ".willBeDelete");
            if (std::filesystem::exists(marker))
            {
                continue;
            }

            auto manifest_path = FindManifest(entry.path());
            if (manifest_path.empty())
            {
                continue;
            }

            try
            {
                std::ifstream     ifs(manifest_path);
                auto              json_data = nlohmann::json::parse(ifs);
                PluginPackageDesc desc;
                from_json(json_data, desc);

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

                loaded_plugin_paths_.push_back(manifest_path);
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
            Das::PluginInterface::IDasTask* task = nullptr;
            auto qi_result = feature_info->interface_ptr->QueryInterface(
                DasIidOf<Das::PluginInterface::IDasTask>(),
                reinterpret_cast<void**>(&task));
            if (!IsOk(qi_result) || !task)
            {
                continue;
            }

            auto type_record = std::make_unique<TaskTypeRecord>();
            type_record->task_instance = task;
            type_record->plugin_guid = feature_info->plugin_guid;

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

            task_types_.push_back(std::move(type_record));
        }

        DAS_CORE_LOG_INFO(
            "SchedulerService::Initialize: discovered {} task types",
            task_types_.size());

        // Materialize persisted queued task instances from profile state
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  scheduler_index = settings.GetSchedulerIndexJson("0");

        if (scheduler_index.contains("taskOrder")
            && scheduler_index["taskOrder"].is_array())
        {
            for (const auto& task_id_val : scheduler_index["taskOrder"])
            {
                if (!task_id_val.is_number_integer())
                {
                    continue;
                }
                int64_t tid = task_id_val.get<int64_t>();

                auto instance_json = settings.GetTaskInstanceJson("0", tid);

                TaskInstanceRecord rec;
                rec.id = tid;

                // Check if the file was valid
                if (instance_json.is_null() || !instance_json.is_object()
                    || !instance_json.contains("taskGuid"))
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason =
                        "task instance file is missing or corrupt";
                    task_instances_.push_back(std::move(rec));
                    continue;
                }

                // Parse taskGuid
                try
                {
                    auto tg_str = instance_json["taskGuid"].get<std::string>();
                    rec.task_guid =
                        Das::Core::ForeignInterfaceHost::MakeDasGuid(tg_str);
                }
                catch (const std::exception& e)
                {
                    rec.availability = TaskAvailability::Invalid;
                    rec.unavailability_reason =
                        std::string("invalid taskGuid: ") + e.what();
                    task_instances_.push_back(std::move(rec));
                    continue;
                }

                // Parse pluginGuid
                if (instance_json.contains("pluginGuid"))
                {
                    try
                    {
                        auto pg_str =
                            instance_json["pluginGuid"].get<std::string>();
                        rec.plugin_guid =
                            Das::Core::ForeignInterfaceHost::MakeDasGuid(
                                pg_str);
                    }
                    catch (const std::exception&)
                    {
                        // Keep default empty guid
                    }
                }

                // Parse nextExecutionTime
                if (instance_json.contains("nextExecutionTime")
                    && !instance_json["nextExecutionTime"].is_null())
                {
                    try
                    {
                        rec.next_execution_time =
                            instance_json["nextExecutionTime"]
                                .get<std::string>();
                    }
                    catch (const std::exception&)
                    {
                        // Keep nullopt
                    }
                }

                // Parse properties
                if (instance_json.contains("properties")
                    && instance_json["properties"].is_object())
                {
                    rec.properties = instance_json["properties"];
                }
                else
                {
                    rec.properties = nlohmann::json::object();
                }

                // Link to available task type
                auto* type_rec = FindTaskType(rec.task_guid);
                if (type_rec)
                {
                    rec.availability = TaskAvailability::Available;
                    rec.task_type = type_rec;
                    rec.plugin_guid = type_rec->plugin_guid;
                }
                else
                {
                    rec.availability = TaskAvailability::Unavailable;
                    rec.unavailability_reason =
                        "task type not found among loaded plugins";
                }

                task_instances_.push_back(std::move(rec));
            }
        }

        initialized_ = true;

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

        tick_timer_ = std::make_unique<boost::asio::steady_timer>(
            ipc_context_.get().GetIoContext());

        StartTickTimer();

        DAS_CORE_LOG_INFO("SchedulerService::Enable: scheduler started");
        return DAS_S_OK;
    }

    DasResult SchedulerService::Disable()
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (state_.load() != SchedulerState::Running)
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Disable: invalid state, must be "
                "Running");
            return DAS_E_FAIL;
        }

        state_.store(SchedulerState::Stopped);

        // Cancel timer to stop new OnTick dispatches
        if (tick_timer_)
        {
            tick_timer_->cancel();
            tick_timer_.reset();
        }

        // Wait for current task to complete
        cv_.wait(lock, [this] { return current_task_ == nullptr; });

        // Unload plugins in reverse order
        for (auto it = loaded_plugin_paths_.rbegin();
             it != loaded_plugin_paths_.rend();
             ++it)
        {
            auto unload_result = plugin_manager_.UnloadPlugin(*it);
            if (DAS::IsFailed(unload_result))
            {
                DAS_CORE_LOG_WARN(
                    "SchedulerService::Disable: failed to unload "
                    "plugin {}, result={}",
                    it->string(),
                    unload_result);
            }
        }

        task_types_.clear();
        task_instances_.clear();
        loaded_plugin_paths_.clear();
        initialized_ = false;

        DAS_CORE_LOG_INFO("SchedulerService::Disable: scheduler stopped");
        return DAS_S_OK;
    }

    SchedulerState SchedulerService::Status() const { return state_.load(); }

    // ----------------------------------------------------------------
    // Get: merged scheduler state JSON
    // ----------------------------------------------------------------

    nlohmann::json SchedulerService::Get()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        nlohmann::json result;
        result["state"] =
            state_.load() == SchedulerState::Running ? "running" : "stopped";

        // Available task types
        auto& types_arr = result["availableTaskTypes"];
        types_arr = nlohmann::json::array();
        for (const auto& ttr : task_types_)
        {
            nlohmann::json type_obj;
            type_obj["taskGuid"] = GuidToString(ttr->task_guid);
            type_obj["pluginGuid"] = GuidToString(ttr->plugin_guid);
            type_obj["name"] = ttr->name;
            type_obj["description"] = ttr->description;
            if (ttr->game_name)
            {
                type_obj["gameName"] = *ttr->game_name;
            }

            // Include descriptors
            auto& desc_arr = type_obj["descriptors"];
            desc_arr = nlohmann::json::array();
            for (const auto& d : ttr->descriptors)
            {
                nlohmann::json desc_obj;
                desc_obj["name"] = d.name;
                desc_obj["type"] = static_cast<int>(d.type);
                desc_obj["required"] = d.required;
                if (d.description)
                {
                    desc_obj["description"] = *d.description;
                }
                desc_arr.push_back(desc_obj);
            }

            types_arr.push_back(type_obj);
        }

        // Queued task instances
        auto& tasks_arr = result["tasks"];
        tasks_arr = nlohmann::json::array();
        for (const auto& inst : task_instances_)
        {
            nlohmann::json task_obj;
            task_obj["id"] = inst.id;
            task_obj["taskGuid"] = GuidToString(inst.task_guid);
            task_obj["pluginGuid"] = GuidToString(inst.plugin_guid);

            if (inst.availability == TaskAvailability::Available)
            {
                task_obj["availability"] = "available";
            }
            else if (inst.availability == TaskAvailability::Unavailable)
            {
                task_obj["availability"] = "unavailable";
                task_obj["unavailabilityReason"] = inst.unavailability_reason;
            }
            else
            {
                task_obj["availability"] = "invalid";
                task_obj["unavailabilityReason"] = inst.unavailability_reason;
            }

            if (inst.next_execution_time)
            {
                task_obj["nextExecutionTime"] = *inst.next_execution_time;
            }
            else
            {
                task_obj["nextExecutionTime"] = nullptr;
            }

            task_obj["properties"] = inst.properties;
            tasks_arr.push_back(task_obj);
        }

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

        std::lock_guard<std::mutex> lock(mutex_);

        if (state_.load() == SchedulerState::Running)
        {
            return DAS_E_TASK_WORKING;
        }

        if (!initialized_)
        {
            return DAS_E_OBJECT_NOT_INIT;
        }

        // Find task type
        auto* type_rec = FindTaskType(task_guid);
        if (!type_rec)
        {
            return DAS_E_NOT_FOUND;
        }

        // Allocate next task id
        auto&   settings = plugin_manager_.GetSettingsManager();
        auto    scheduler_index = settings.GetSchedulerIndexJson("0");
        int64_t next_id = scheduler_index.value("nextTaskId", 0);

        // Build instance with descriptor defaults
        TaskInstanceRecord rec;
        rec.id = next_id;
        rec.task_guid = task_guid;
        rec.plugin_guid = type_rec->plugin_guid;
        rec.availability = TaskAvailability::Available;
        rec.task_type = type_rec;
        rec.properties = nlohmann::json::object();

        // Initialize properties from descriptor defaults
        for (const auto& desc : type_rec->descriptors)
        {
            if (!std::holds_alternative<std::monostate>(desc.default_value))
            {
                std::visit(
                    [&](const auto& val)
                    {
                        if constexpr (!std::is_same_v<
                                          std::decay_t<decltype(val)>,
                                          std::monostate>)
                        {
                            rec.properties[desc.name] = val;
                        }
                    },
                    desc.default_value);
            }
        }

        // Persist task instance file
        nlohmann::json instance_json;
        instance_json["id"] = rec.id;
        instance_json["taskGuid"] = GuidToString(rec.task_guid);
        instance_json["pluginGuid"] = GuidToString(rec.plugin_guid);
        instance_json["nextExecutionTime"] = nullptr;
        instance_json["properties"] = rec.properties;

        auto persist_result =
            settings.UpdateTaskInstanceJson("0", rec.id, instance_json);
        if (IsFailed(persist_result))
        {
            return persist_result;
        }

        // Update scheduler index
        scheduler_index["nextTaskId"] = next_id + 1;
        scheduler_index["taskOrder"].push_back(next_id);
        auto index_result =
            settings.UpdateSchedulerIndexJson("0", scheduler_index);
        if (IsFailed(index_result))
        {
            // Rollback task instance file
            settings.DeleteTaskInstanceJson("0", rec.id);
            return index_result;
        }

        task_instances_.push_back(std::move(rec));
        *out_task_id = next_id;

        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // DeleteTask
    // ----------------------------------------------------------------

    DasResult SchedulerService::DeleteTask(int64_t task_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_.load() == SchedulerState::Running)
        {
            return DAS_E_TASK_WORKING;
        }

        // Find and remove instance
        auto it = std::find_if(
            task_instances_.begin(),
            task_instances_.end(),
            [task_id](const TaskInstanceRecord& r) { return r.id == task_id; });

        if (it == task_instances_.end())
        {
            return DAS_E_NOT_FOUND;
        }

        task_instances_.erase(it);

        // Update persisted state
        auto& settings = plugin_manager_.GetSettingsManager();

        // Delete task instance file
        settings.DeleteTaskInstanceJson("0", task_id);

        // Update scheduler index (remove from taskOrder)
        auto scheduler_index = settings.GetSchedulerIndexJson("0");
        if (scheduler_index.contains("taskOrder")
            && scheduler_index["taskOrder"].is_array())
        {
            auto& order = scheduler_index["taskOrder"];
            for (auto oit = order.begin(); oit != order.end(); ++oit)
            {
                if (oit->is_number_integer() && oit->get<int64_t>() == task_id)
                {
                    order.erase(oit);
                    break;
                }
            }
        }
        settings.UpdateSchedulerIndexJson("0", scheduler_index);

        return DAS_S_OK;
    }

    // ----------------------------------------------------------------
    // UpdateTaskProperties
    // ----------------------------------------------------------------

    DasResult SchedulerService::UpdateTaskProperties(
        int64_t               task_id,
        const nlohmann::json& properties)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_.load() == SchedulerState::Running)
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

        // Validate property names and types against descriptors
        auto* type_rec = inst->task_type;
        if (!type_rec)
        {
            return DAS_E_FAIL;
        }

        if (!properties.is_object())
        {
            return DAS_E_INVALID_JSON;
        }

        for (auto it = properties.begin(); it != properties.end(); ++it)
        {
            const auto& prop_name = it.key();
            bool        found = false;
            for (const auto& desc : type_rec->descriptors)
            {
                if (desc.name == prop_name)
                {
                    // Type validation
                    switch (desc.type)
                    {
                    case Das::ExportInterface::DAS_TYPE_BOOL:
                        if (!it->is_boolean())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_INT:
                        if (!it->is_number_integer())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_FLOAT:
                        if (!it->is_number())
                        {
                            return DAS_E_TYPE_ERROR;
                        }
                        break;
                    case Das::ExportInterface::DAS_TYPE_STRING:
                        if (!it->is_string())
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

        // Apply validated properties
        for (auto it = properties.begin(); it != properties.end(); ++it)
        {
            inst->properties[it.key()] = it.value();
        }

        // Persist
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  instance_json = settings.GetTaskInstanceJson("0", task_id);
        if (instance_json.is_null() || !instance_json.is_object())
        {
            instance_json = nlohmann::json::object();
        }
        instance_json["properties"] = inst->properties;
        return settings.UpdateTaskInstanceJson("0", task_id, instance_json);
    }

    // ----------------------------------------------------------------
    // UpdateTaskInternalProperties
    // ----------------------------------------------------------------

    DasResult SchedulerService::UpdateTaskInternalProperties(
        int64_t               task_id,
        const nlohmann::json& internal_props)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_.load() == SchedulerState::Running)
        {
            return DAS_E_TASK_WORKING;
        }

        auto* inst = FindTaskInstance(task_id);
        if (!inst)
        {
            return DAS_E_NOT_FOUND;
        }

        // Update nextExecutionTime if provided
        if (internal_props.contains("nextExecutionTime"))
        {
            const auto& val = internal_props["nextExecutionTime"];
            if (val.is_null())
            {
                inst->next_execution_time = std::nullopt;
            }
            else if (val.is_string())
            {
                inst->next_execution_time = val.get<std::string>();
            }
            else
            {
                return DAS_E_TYPE_ERROR;
            }
        }

        // Persist
        auto& settings = plugin_manager_.GetSettingsManager();
        auto  instance_json = settings.GetTaskInstanceJson("0", task_id);
        if (instance_json.is_null() || !instance_json.is_object())
        {
            instance_json = nlohmann::json::object();
        }
        if (inst->next_execution_time)
        {
            instance_json["nextExecutionTime"] = *inst->next_execution_time;
        }
        else
        {
            instance_json["nextExecutionTime"] = nullptr;
        }
        return settings.UpdateTaskInstanceJson("0", task_id, instance_json);
    }

    // ----------------------------------------------------------------
    // Timer and tick
    // ----------------------------------------------------------------

    void SchedulerService::StartTickTimer()
    {
        if (!tick_timer_)
        {
            return;
        }

        tick_timer_->expires_after(std::chrono::seconds(1));
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

        DAS_CORE_LOG_DEBUG("SchedulerService::OnTick: tick");
        StartTickTimer();
    }

} // namespace Das::Core::TaskScheduler
