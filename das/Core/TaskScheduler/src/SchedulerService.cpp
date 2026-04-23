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

    SchedulerService::SchedulerService(
        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager,
        Das::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context)
        : plugin_manager_(plugin_manager), ipc_context_{std::move(ipc_context)}
    {
    }

    DasResult SchedulerService::Initialize(
        const std::filesystem::path& plugin_dir,
        const std::vector<DasGuid>&  disabled_guids)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ != SchedulerState::Stopped)
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Initialize: invalid state, "
                "must be Stopped");
            return DAS_E_FAIL;
        }

        if (!tasks_.empty())
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Initialize: already initialized");
            return DAS_E_OBJECT_ALREADY_INIT;
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

        // Collect IDasTask instances from loaded plugins
        auto task_features = plugin_manager_.GetFeaturesByType(
            Das::PluginInterface::DAS_PLUGIN_FEATURE_TASK);
        for (auto* feature_info : task_features)
        {
            Das::PluginInterface::IDasTask* task = nullptr;
            auto qi_result = feature_info->interface_ptr->QueryInterface(
                DasIidOf<Das::PluginInterface::IDasTask>(),
                reinterpret_cast<void**>(&task));
            if (IsOk(qi_result) && task)
            {
                tasks_.emplace_back(task);
            }
        }

        DAS_CORE_LOG_INFO(
            "SchedulerService::Initialize: loaded {} tasks",
            tasks_.size());

        return DAS_S_OK;
    }

    DasResult SchedulerService::Enable()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (state_ != SchedulerState::Stopped)
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Enable: invalid state, must be "
                "Stopped");
            return DAS_E_FAIL;
        }

        if (tasks_.empty())
        {
            DAS_CORE_LOG_ERROR(
                "SchedulerService::Enable: no tasks loaded, call "
                "Initialize first");
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

        if (state_ != SchedulerState::Running)
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

        // Wait for current task to complete (D-13)
        // In Phase 49, current_task_ is always nullptr, so this
        // passes immediately
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

        tasks_.clear();
        loaded_plugin_paths_.clear();

        DAS_CORE_LOG_INFO("SchedulerService::Disable: scheduler stopped");
        return DAS_S_OK;
    }

    SchedulerState SchedulerService::Status() const { return state_.load(); }

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

        // Phase 49: only reschedule timer
        // Phase 50: add GetNextExecutionTime sorting + ExecuteNextTask
        StartTickTimer();
    }

} // namespace Das::Core::TaskScheduler
