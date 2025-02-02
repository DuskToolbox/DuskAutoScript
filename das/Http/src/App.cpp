#include <iostream>
#include <ostream>

#include "oatpp/network/Server.hpp"
#include "das/Utils/ThreadUtils.h"

#include "./AppComponent.hpp"
#include "./controller/DasLogController.hpp"
#include "./controller/DasMiscController.hpp"
#include "./controller/DasPluginManagerController.hpp"
#include "./controller/DasProfileController.hpp"
#include "./controller/UISettingsController.hpp"

namespace Das::Http
{
    DAS_DEFINE_VARIABLE(g_server_condition){};

    void ServerCondition::RequestServerStop()
    {
        server_should_continue_ = false;
    }

    std::function<bool()> ServerCondition::GetCondition()
    {
        return [this]() -> bool { return server_should_continue_; };
    }
}

void run()
{
    AppComponent components;

    OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router);

    InitializeGlobalTaskScheduler();

    // 初始化基础api
    router->addController(std::make_shared<DasMiscController>());
    // 初始化Logger
    router->addController(std::make_shared<DasLogController>());
    // 从后端取回设置json
    router->addController(std::make_shared<DasProfileManagerController>());
    // 初始化Core
    router->addController(std::make_shared<DasPluginManagerController>());
    router->addController(std::make_shared<DasUiSettingsController>());

    OATPP_COMPONENT(
        std::shared_ptr<oatpp::network::ConnectionHandler>,
        connectionHandler);

    OATPP_COMPONENT(
        std::shared_ptr<oatpp::network::ServerConnectionProvider>,
        connectionProvider);

    oatpp::network::Server server(connectionProvider, connectionHandler);

    OATPP_LOGI(
        "DuskAutoScriptHttp",
        "Server running on port %s",
        connectionProvider->getProperty("port").getData());

    server.run(DAS::Http::g_server_condition.GetCondition());

    /* Server has shut down, so we dont want to connect any new connections */
    connectionProvider->stop();

    /* Now stop the connection handler and wait until all running connections
     * are served */
    connectionHandler->stop();
}

int main(int argc, const char* argv[])
{
    std::cout << "[DasHttp] " << (argv[0] ? argv[0] : "") << " is start"
              << std::endl;

    DAS::Utils::SetCurrentThreadName(L"MAIN");

    oatpp::base::Environment::init();

    run();

    std::cout << "\nEnvironment:\n";
    std::cout << "objectsCount = "
              << oatpp::base::Environment::getObjectsCount() << "\n";
    std::cout << "objectsCreated = "
              << oatpp::base::Environment::getObjectsCreated() << "\n\n";

    oatpp::base::Environment::destroy();

    return 0;
}