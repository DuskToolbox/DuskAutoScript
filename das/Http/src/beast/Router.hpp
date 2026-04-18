#ifndef DAS_HTTP_BEAST_ROUTER_HPP
#define DAS_HTTP_BEAST_ROUTER_HPP

#include "Request.hpp"
#include <das/IDasBase.h>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace Das::Http::Beast
{

    // 处理器函数类型
    using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

    // Path parameter parsing helpers
    static std::vector<std::string> SplitPath(const std::string& path)
    {
        std::vector<std::string> segments;
        std::string              current;
        for (char c : path)
        {
            if (c == '/')
            {
                if (!current.empty())
                {
                    segments.push_back(current);
                    current.clear();
                }
            }
            else
            {
                current += c;
            }
        }
        if (!current.empty())
        {
            segments.push_back(current);
        }
        return segments;
    }

    static bool IsParamSegment(const std::string& segment)
    {
        return !segment.empty() && segment.front() == '{'
               && segment.back() == '}';
    }

    static std::string ExtractParamName(const std::string& segment)
    {
        // "{pid}" -> "pid"
        return segment.substr(1, segment.size() - 2);
    }

    // Parameterized route entry
    struct ParamRoute
    {
        std::string              method;
        std::vector<std::string> segments;
        std::vector<size_t>      param_indices;
        RouteHandler             handler;
    };

    // 路由器
    class Router
    {
    public:
        Router() = default;

        // 注册路由
        void Register(
            const std::string& method,
            const std::string& path,
            RouteHandler       handler)
        {
            // Check if path contains parameter placeholders
            if (path.find('{') != std::string::npos)
            {
                ParamRoute param_route;
                param_route.method = method;
                param_route.segments = SplitPath(path);
                param_route.handler = std::move(handler);

                for (size_t i = 0; i < param_route.segments.size(); ++i)
                {
                    if (IsParamSegment(param_route.segments[i]))
                    {
                        param_route.param_indices.push_back(i);
                    }
                }

                param_routes_.push_back(std::move(param_route));
                return;
            }

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
            std::string key = std::string(request.Method()) + ":"
                              + std::string(request.Target());

            // Try exact match first
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
                        DAS_E_INTERNAL_FATAL_ERROR,
                        "Internal server error");
                }
            }

            // Try parameterized routes
            std::string              method = std::string(request.Method());
            std::vector<std::string> request_segments =
                SplitPath(std::string(request.Target()));

            for (const auto& param_route : param_routes_)
            {
                if (param_route.method != method)
                {
                    continue;
                }

                if (param_route.segments.size() != request_segments.size())
                {
                    continue;
                }

                bool match = true;
                for (size_t i = 0; i < param_route.segments.size(); ++i)
                {
                    if (IsParamSegment(param_route.segments[i]))
                    {
                        // Extract parameter value
                        std::string param_name =
                            ExtractParamName(param_route.segments[i]);
                        request.SetPathParameter(
                            param_name,
                            request_segments[i]);
                    }
                    else if (param_route.segments[i] != request_segments[i])
                    {
                        match = false;
                        break;
                    }
                }

                if (match)
                {
                    try
                    {
                        return param_route.handler(request);
                    }
                    catch (...)
                    {
                        return HttpResponse::CreateErrorResponse(
                            DAS_E_INTERNAL_FATAL_ERROR,
                            "Internal server error");
                    }
                }
            }

            // 未找到路由
            HttpResponse   response(http::status::not_found);
            nlohmann::json body;
            body["code"] = DAS_E_FILE_NOT_FOUND;
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
        std::vector<ParamRoute>                       param_routes_;
    };

} // namespace Das::Http::Beast

#endif // DAS_HTTP_BEAST_ROUTER_HPP
