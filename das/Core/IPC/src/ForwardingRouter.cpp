#include "das/Core/IPC/ForwardingRouter.h"

#include <das/Core/IPC/IpcMessageHeader.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        ForwardingRouter::ForwardingRouter()
            : successful_routes_count_(0), failed_routes_count_(0)
        {
        }

        ForwardingRouter::~ForwardingRouter() { ClearRoutes(); }

        bool ForwardingRouter::AddRoute(
            const RouteKey&    key,
            const RouteTarget& target)
        {
            if (!ValidateTarget(target))
            {
                return false;
            }

            auto it = route_table_.find(key);
            if (it != route_table_.end())
            {
                // 路由已存在，更新它
                it->second = target;
            }
            else
            {
                // 添加新路由
                route_table_[key] = target;
            }
            return true;
        }

        bool ForwardingRouter::RemoveRoute(const RouteKey& key)
        {
            auto it = route_table_.find(key);
            if (it != route_table_.end())
            {
                route_table_.erase(it);
                return true;
            }
            return false;
        }

        void ForwardingRouter::ClearRoutes() { route_table_.clear(); }

        size_t ForwardingRouter::GetRouteCount() const
        {
            return route_table_.size();
        }

        bool ForwardingRouter::HasRoute(const RouteKey& key) const
        {
            return route_table_.find(key) != route_table_.end();
        }

        bool ForwardingRouter::FindTarget(
            const RouteKey& key,
            RouteTarget&    out_target) const
        {
            auto it = route_table_.find(key);
            if (it != route_table_.end())
            {
                out_target = it->second;
                return true;
            }
            return false;
        }

        std::vector<RouteTarget> ForwardingRouter::FindAllTargets() const
        {
            std::vector<RouteTarget> targets;
            targets.reserve(route_table_.size());

            for (const auto& pair : route_table_)
            {
                targets.push_back(pair.second);
            }

            return targets;
        }

        ForwardingRouter::RouteResult ForwardingRouter::RouteMessage(
            const IPCMessageHeader& header,
            const std::vector<uint8_t>& /*payload*/)
        {
            RouteResult result;
            RouteKey    key = CreateRouteKey(header);

            auto it = route_table_.find(key);
            if (it != route_table_.end())
            {
                result.success = true;
                result.target = it->second;
                successful_routes_count_++;
            }
            else
            {
                result.success = false;
                result.target = RouteTarget();
                result.error_message = "No route found for the given key";
                failed_routes_count_++;
            }

            return result;
        }

        ForwardingRouter::RouteStats ForwardingRouter::GetStats() const
        {
            RouteStats stats;
            stats.total_routes = GetRouteCount();
            stats.successful_routes = successful_routes_count_;
            stats.failed_routes = failed_routes_count_;
            return stats;
        }

        bool ForwardingRouter::ValidateTarget(const RouteTarget& target) const
        {
            return target.is_valid && target.session_id != 0
                   && target.object_id != 0 && target.interface_id != 0;
        }

        RouteKey ForwardingRouter::CreateRouteKey(
            const IPCMessageHeader& header) const
        {
            return RouteKey(header.object_id, header.interface_id);
        }

    }
}
DAS_NS_END