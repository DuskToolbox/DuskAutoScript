#ifndef DAS_CORE_IPC_FORWARDING_ROUTER_H
#define DAS_CORE_IPC_FORWARDING_ROUTER_H

#include <cstdint>
#include <das/IDasBase.h>
#include <string>
#include <unordered_map>
#include <vector>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        struct IPCMessageHeader;

        struct RouteTarget
        {
            uint64_t session_id;   // 会话ID
            uint64_t object_id;    // 对象ID
            uint32_t interface_id; // 接口ID
            DasGuid  type_id;      // 类型ID
            bool     is_valid;     // 路由是否有效

            RouteTarget()
                : session_id(0), object_id(0), interface_id(0), is_valid(false)
            {
            }

            RouteTarget(
                uint64_t       sid,
                uint64_t       oid,
                uint32_t       iid,
                const DasGuid& tid)
                : session_id(sid), object_id(oid), interface_id(iid),
                  type_id(tid), is_valid(true)
            {
            }
        };

        struct RouteKey
        {
            uint16_t session_id;   // 会话ID
            uint16_t generation;   // 对象版本
            uint32_t local_id;     // 本地对象ID
            uint32_t interface_id; // 接口ID

            RouteKey()
                : session_id(0), generation(0), local_id(0), interface_id(0)
            {
            }

            RouteKey(uint16_t sid, uint16_t gen, uint32_t lid, uint32_t iid)
                : session_id(sid), generation(gen), local_id(lid),
                  interface_id(iid)
            {
            }

            bool operator==(const RouteKey& other) const
            {
                return session_id == other.session_id
                       && generation == other.generation
                       && local_id == other.local_id
                       && interface_id == other.interface_id;
            }

            size_t hash() const
            {
                return static_cast<size_t>(session_id)
                       | (static_cast<size_t>(generation) << 16)
                       | (static_cast<size_t>(local_id) << 32)
                       | (static_cast<size_t>(interface_id) << 48);
            }
        };

        struct RouteKeyHash
        {
            size_t operator()(const RouteKey& key) const { return key.hash(); }
        };

        class ForwardingRouter
        {
        public:
            ForwardingRouter();
            virtual ~ForwardingRouter();

            bool   AddRoute(const RouteKey& key, const RouteTarget& target);
            bool   RemoveRoute(const RouteKey& key);
            void   ClearRoutes();
            size_t GetRouteCount() const;
            bool   HasRoute(const RouteKey& key) const;

            bool FindTarget(const RouteKey& key, RouteTarget& out_target) const;
            std::vector<RouteTarget> FindAllTargets() const;

            struct RouteResult
            {
                bool        success;
                RouteTarget target;
                std::string error_message;
            };

            RouteResult RouteMessage(
                const IPCMessageHeader&     header,
                const std::vector<uint8_t>& payload);

            struct RouteStats
            {
                size_t total_routes;
                size_t successful_routes;
                size_t failed_routes;
            };

            RouteStats GetStats() const;

        private:
            std::unordered_map<RouteKey, RouteTarget, RouteKeyHash>
                           route_table_;
            mutable size_t successful_routes_count_;
            mutable size_t failed_routes_count_;

            bool     ValidateTarget(const RouteTarget& target) const;
            RouteKey CreateRouteKey(const IPCMessageHeader& header) const;
        };

    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_FORWARDING_ROUTER_H