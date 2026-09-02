#ifndef DAS_HTTP_BEAST_ROUTER_HPP
#define DAS_HTTP_BEAST_ROUTER_HPP

#include "Request.hpp"
#include <cpp_yyjson.hpp>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <das/Utils/fmt.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_STDEXEC_PARAMETERS
#include <exec/async_scope.hpp>
DAS_DISABLE_WARNING_END

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

    class Router
    {
    public:
        using BusinessScheduler = DAS::Core::IPC::BusinessThreadScheduler<
            DAS::Core::IPC::MainProcess::IIpcContext>;

        Router() = default;

        explicit Router(BusinessScheduler scheduler) : bt_scheduler_(scheduler)
        {
        }

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
            std::string target = std::string(request.Target());
            // 日志端点（/logs/）静默：避免"处理日志请求 → 写 [HTTP] 日志 →
            // DasLogNotifierSink 置 dirty → log tick broadcast →
            // 前端又发日志请求" 的反馈循环。其他端点照常记请求日志。
            if (target.find("/logs/") == std::string::npos)
            {
                DAS_LOG_INFO(
                    DAS_FMT_NS::format("[HTTP] {} {}", request.Method(), target)
                        .c_str());
            }
            if (!target.empty() && target.front() == '/')
            {
                target = target.substr(1);
            }
            std::string key = std::string(request.Method()) + ":" + target;

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
            HttpResponse response(http::status::not_found);
            auto         body = Das::Utils::MakeYyjsonObject();
            auto         body_obj_opt = body.as_object();
            if (body_obj_opt)
            {
                auto& body_obj = body_obj_opt.value();
                body_obj["code"] = static_cast<int64_t>(DAS_E_FILE_NOT_FOUND);
                body_obj["message"] = std::string("Route not found");
                body_obj["data"] = yyjson::value{};
            }
            response.SetBody(body);
            return response;
        }

        // 检查路由是否存在
        bool HasRoute(const std::string& method, const std::string& path) const
        {
            std::string key = method + ":" + path;
            return routes_.find(key) != routes_.end();
        }

        // 标记路由留在 io 线程内联（alive 探针须在业务线程卡死时仍可应答；
        // request_shutdown 停机触发耦合在响应写完成回调）
        void RegisterInline(const std::string& method, const std::string& path)
        {
            inline_routes_.insert(method + ":" + path);
        }

        // 键构造与 Handle 的精确匹配同构；参数路由与未匹配返回 false
        bool IsInlineRoute(const HttpRequest& request) const
        {
            if (inline_routes_.empty())
            {
                return false;
            }
            std::string target = std::string(request.Target());
            if (!target.empty() && target.front() == '/')
            {
                target = target.substr(1);
            }
            const std::string key =
                std::string(request.Method()) + ":" + target;
            return inline_routes_.find(key) != inline_routes_.end();
        }

        bool DispatchToBusiness(const HttpRequest& request) const
        {
            return bt_scheduler_.ctx != nullptr && !IsInlineRoute(request);
        }

        BusinessScheduler GetBusinessScheduler() const { return bt_scheduler_; }

        template <typename Sender>
        void SpawnBusiness(Sender&& sender)
        {
            business_scope_.spawn(std::forward<Sender>(sender));
        }

        /**
         * @brief 停机排空：业务完成回调会向 io_context post，ioc 析构前
         *        必须等完。等待必然终止——BT 在途工作不依赖 HTTP io
         *        执行与 MAIN。
         */
        void WaitBusinessDrained()
        {
            static_cast<void>(stdexec::sync_wait(business_scope_.on_empty()));
        }

    private:
        std::unordered_map<std::string, RouteHandler> routes_;
        std::vector<ParamRoute>                       param_routes_;
        std::unordered_set<std::string>               inline_routes_;
        // ctx == nullptr = 不派发，全内联
        BusinessScheduler bt_scheduler_{nullptr};
        exec::async_scope business_scope_;
    };

} // namespace Das::Http::Beast

#endif // DAS_HTTP_BEAST_ROUTER_HPP
