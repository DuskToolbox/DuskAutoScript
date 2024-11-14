#include <iostream>
#include <ostream>

#include "oatpp/network/Server.hpp"

#include "./AppComponent.hpp"
#include "./controller/Controller.hpp"

void run()
{
    AppComponent components;

    OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router);

    router->addController(std::make_shared<Controller>());

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

    server.run();
}

int main(int argc, const char* argv[])
{

    std::cout << "is start" << std::endl;

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