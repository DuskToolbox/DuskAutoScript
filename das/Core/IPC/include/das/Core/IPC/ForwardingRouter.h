#ifndef DAS_CORE_IPC_FORWARDING_ROUTER_H
#define DAS_CORE_IPC_FORWARDING_ROUTER_H

#include <cstdint>
#include <das/IDasBase.h>
#include <string>
#include <unordered_map>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN
struct IPCMessageHeader;

struct RouteTarget
{
    uint16_t session_id;   // 目标 session ID (V3: target_session_id)
    uint64_t object_id;    // 对象ID (在 body 中携带)
    uint32_t interface_id; // 接口ID
    DasGuid  type_id;      // 类型ID
    bool     is_valid;     // 路由是否有效

    RouteTarget()
        : session_id(0), object_id(0), interface_id(0), is_valid(false)
    {
    }

    RouteTarget(uint16_t sid, uint64_t oid, uint32_t iid, const DasGuid& tid)
        : session_id(sid), object_id(oid), interface_id(iid), type_id(tid),
          is_valid(true)
    {
    }
};

/// @brief V3 路由键 - 使用 target_session_id + interface_id 进行路由
struct RouteKey
{
    uint16_t target_session_id; // 目标 session ID (V3 Header 字段)
    uint32_t interface_id;      // 接口ID

    RouteKey() : target_session_id(0), interface_id(0) {}

    RouteKey(uint16_t tsid, uint32_t iid)
        : target_session_id(tsid), interface_id(iid)
    {
    }

    bool operator==(const RouteKey& other) const
    {
        return target_session_id == other.target_session_id
               && interface_id == other.interface_id;
    }

    size_t hash() const
    {
        return static_cast<size_t>(target_session_id)
               | (static_cast<size_t>(interface_id) << 16);
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
    std::unordered_map<RouteKey, RouteTarget, RouteKeyHash> route_table_;
    mutable size_t successful_routes_count_;
    mutable size_t failed_routes_count_;

    bool     ValidateTarget(const RouteTarget& target) const;
    RouteKey CreateRouteKey(const IPCMessageHeader& header) const;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_FORWARDING_ROUTER_H
