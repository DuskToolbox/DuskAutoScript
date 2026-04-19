#ifndef DAS_HTTP_APP_COMPONENT_HPP
#define DAS_HTTP_APP_COMPONENT_HPP

#include "beast/Router.hpp"
#include "beast/Server.hpp"
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace Das::Http
{
    class AppComponent
    {
    public:
        std::shared_ptr<Beast::Router>              router;
        std::function<bool()>                       stop_condition;
        Das::Core::SettingsManager::SettingsManager settings_manager;
        std::filesystem::path                       plugin_dir;

        // IPC context (process-level) — per D-17/D-18
        std::shared_ptr<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context;

        // IPC event loop thread
        std::thread ipc_thread;

        explicit AppComponent(const std::filesystem::path& plugin_dir)
            : router(std::make_shared<Beast::Router>()),
              settings_manager(std::filesystem::path("settings")),
              plugin_dir(plugin_dir)
        {
        }
    };

} // namespace Das::Http

#endif // DAS_HTTP_APP_COMPONENT_HPP
