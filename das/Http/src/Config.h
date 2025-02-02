#ifndef DAS_HTTP_CONFIG_H
#define DAS_HTTP_CONFIG_H

#define DAS_HTTP_API_PREFIX "api/v1/"

#include <atomic>
#include <functional>

namespace Das::Http
{
    class ServerCondition
    {
        std::atomic_bool server_should_continue_{true};

    public:
        void                  RequestServerStop();
        std::function<bool()> GetCondition();
    };
    extern ServerCondition g_server_condition;
}

#endif // DAS_HTTP_CONFIG_H
