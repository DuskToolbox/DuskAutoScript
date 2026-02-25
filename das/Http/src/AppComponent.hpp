#ifndef DAS_HTTP_APP_COMPONENT_HPP
#define DAS_HTTP_APP_COMPONENT_HPP

#include "beast/Router.hpp"
#include "beast/Server.hpp"
#include <string>

namespace Das::Http
{

    class AppComponent
    {
    public:
        std::shared_ptr<Beast::Router> router;
        std::function<bool()>          stop_condition;

        AppComponent() { router = std::make_shared<Beast::Router>(); }
    };

} // namespace Das::Http

#endif // DAS_HTTP_APP_COMPONENT_HPP