#pragma once

#include <atomic>
#include <condition_variable>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/DasExport.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncCallback.h>
#include <das/IDasSchedulerService.h>
#include <das/_autogen/idl/abi/IDasPluginPackage.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <memory>
#include <mutex>
#include <vector>

#include <boost/asio/steady_timer.hpp>

namespace Das::Core::TaskScheduler
{

    class DAS_API SchedulerService : public IDasSchedulerService
    {
    public:
        explicit SchedulerService(
            Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager);

        void SetIpcContext(
            DAS::Core::IPC::MainProcess::IIpcContext& ipc_context);

        // IDasSchedulerService
        DasResult Initialize(
            const std::filesystem::path& plugin_dir,
            const std::vector<DasGuid>&  disabled_guids) override;
        DasResult      Enable() override;
        DasResult      Disable() override;
        SchedulerState Status() const override;

    private:
        void StartTickTimer();
        void OnTick();

        // IDasAsyncCallback implementation for PostToBusinessThread dispatch
        class TickCallback final : public IDasAsyncCallback
        {
        public:
            explicit TickCallback(SchedulerService* scheduler)
                : scheduler_(scheduler)
            {
            }

            uint32_t AddRef() override { return ++ref_count_; }

            uint32_t Release() override
            {
                auto count = --ref_count_;
                if (count == 0)
                {
                    delete this;
                }
                return count;
            }

            DasResult QueryInterface(const DasGuid& iid, void** pp) override
            {
                if (iid == DasIidOf<IDasAsyncCallback>())
                {
                    AddRef();
                    *pp = this;
                    return DAS_S_OK;
                }
                return DAS_E_NO_INTERFACE;
            }

            DasResult Do() noexcept override
            {
                if (scheduler_)
                {
                    scheduler_->OnTick();
                }
                return DAS_S_OK;
            }

        private:
            SchedulerService*     scheduler_;
            std::atomic<uint32_t> ref_count_{1};
        };

        Das::Core::ForeignInterfaceHost::PluginManager& plugin_manager_;
        DAS::Core::IPC::MainProcess::IIpcContext*       ipc_context_ = nullptr;

        mutable std::mutex          mutex_;
        std::atomic<SchedulerState> state_{SchedulerState::Stopped};
        std::condition_variable     cv_;

        std::vector<DasPtr<Das::PluginInterface::IDasTask>> tasks_;
        Das::PluginInterface::IDasTask*    current_task_ = nullptr;
        std::vector<std::filesystem::path> loaded_plugin_paths_;

        std::unique_ptr<boost::asio::steady_timer> tick_timer_;
    };

} // namespace Das::Core::TaskScheduler
