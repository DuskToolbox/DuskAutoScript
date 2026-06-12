#pragma once

#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/Host/HostCommandHandlers.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <unordered_map>

namespace Das::Core::IPC::Host
{

    class FakeHostContext final : public IIpcContext
    {
    public:
        FakeHostContext() { object_manager_.SetSessionId(1); }

        DasResult Run() override { return DAS_S_OK; }
        void      RequestStop() override {}
        bool      IsConnected() const override { return true; }

        void RegisterCommandHandler(uint32_t cmd_type, CommandHandler handler)
            override
        {
            handlers[cmd_type] = std::move(handler);
        }

        void PostCallback(IDasAsyncCallback*) override {}

        DasResult RegisterLocalObject(IDasBase* obj, ObjectId& out_id) override
        {
            return object_manager_.RegisterLocalObject(obj, out_id);
        }

        Das::Core::IPC::IDistributedObjectManager& GetObjectManager() override
        {
            return object_manager_;
        }

        DasResult ResolveMainProcessInterface(const DasGuid&, IDasBase**)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult ResolveMainProcessInterfaceByName(const char*, IDasBase**)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        std::unordered_map<uint32_t, CommandHandler> handlers;

    private:
        Das::Core::IPC::DistributedObjectManager object_manager_;
    };

} // namespace Das::Core::IPC::Host
