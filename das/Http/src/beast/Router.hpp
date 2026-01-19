#ifndef DAS_HTTP_BEAST_ROUTER_HPP
#define DAS_HTTP_BEAST_ROUTER_HPP

#include "Request.hpp"
#include <functional>
#include <unordered_map>
#include <string>
#include <memory>

namespace Das::Http::Beast
{

// 处理器函数类型
using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

// 路由器
class Router
{
public:
    Router() = default;
    
    // 注册路由
    void Register(const std::string& method, const std::string& path, RouteHandler handler)
    {
        std::string key = method + ":" + path;
        routes_[key] = std::move(handler);
    }
    
    // POST快捷注册
    void Post(const std::string& path, RouteHandler handler)
    {
        Register("POST", path, std::move(handler));
    }
    
    // GET快捷注册
    void Get(const std::string& path, RouteHandler handler)
    {
        Register("GET", path, std::move(handler));
    }
    
    // PUT快捷注册
    void Put(const std::string& path, RouteHandler handler)
    {
        Register("PUT", path, std::move(handler));
    }
    
    // DELETE快捷注册
    void Delete(const std::string& path, RouteHandler handler)
    {
        Register("DELETE", path, std::move(handler));
    }
    
    // 处理请求
    HttpResponse Handle(const HttpRequest& request) const
    {
        std::string key = std::string(request.Method()) + ":" + std::string(request.Target());
        
        auto it = routes_.find(key);
        if (it != routes_.end())
        {
            try
            {
                return it->second(request);
            }
            catch (...)
            {
                return HttpResponse::CreateErrorResponse(
                    DAS_E_UNEXPECTED, 
                    "Internal server error");
            }
        }
        
        // 未找到路由
        HttpResponse response(http::status::not_found);
        nlohmann::json body;
        body["code"] = DAS_E_NOT_FOUND;
        body["message"] = "Route not found";
        body["data"] = nullptr;
        response.SetBody(body);
        return response;
    }
    
    // 检查路由是否存在
    bool HasRoute(const std::string& method, const std::string& path) const
    {
        std::string key = method + ":" + path;
        return routes_.find(key) != routes_.end();
    }
    
private:
    std::unordered_map<std::string, RouteHandler> routes_;
};

} // namespace Das::Http::Beast

#endif // DAS_HTTP_BEAST_ROUTER_HPP
