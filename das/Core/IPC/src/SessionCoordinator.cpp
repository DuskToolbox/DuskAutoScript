#include <algorithm>
#include <das/Core/IPC/SessionCoordinator.h>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        // 保留的 session_id 定义
        const uint16_t SessionCoordinator::reserved_session_ids_[3] = {
            0,
            1,
            0xFFFF};

        SessionCoordinator& SessionCoordinator::GetInstance()
        {
            static SessionCoordinator instance;
            return instance;
        }

        SessionCoordinator::SessionCoordinator()
        {
            // 初始化保留的 session_id
            for (uint16_t reserved_id : reserved_session_ids_)
            {
                if (reserved_id < MAX_SESSION_IDS)
                {
                    allocated_ids_[reserved_id] = true;
                }
            }
        }

        SessionCoordinator::~SessionCoordinator() = default;

        uint16_t SessionCoordinator::AllocateSessionId()
        {
            std::lock_guard<std::mutex> lock(allocated_ids_mutex_);

            uint16_t available_id = FindAvailableSessionId();
            if (available_id == 0)
            {
                return 0; // 没有可用的 session_id
            }

            MarkSessionIdAsAllocated(available_id);
            return available_id;
        }

        void SessionCoordinator::ReleaseSessionId(uint16_t session_id)
        {
            if (!IsValidSessionId(session_id))
            {
                return;
            }

            std::lock_guard<std::mutex> lock(allocated_ids_mutex_);
            MarkSessionIdAsFree(session_id);
        }

        uint16_t SessionCoordinator::GetLocalSessionId() const
        {
            return local_session_id_.load();
        }

        void SessionCoordinator::SetLocalSessionId(uint16_t session_id)
        {
            if (IsValidSessionId(session_id))
            {
                local_session_id_.store(session_id);
            }
        }

        bool SessionCoordinator::IsValidSessionId(uint16_t session_id)
        {
            // 检查是否为保留的 session_id
            for (uint16_t reserved_id : reserved_session_ids_)
            {
                if (session_id == reserved_id)
                {
                    return false;
                }
            }

            // 检查是否在有效范围内
            return session_id > 0 && session_id < 0xFFFF;
        }

        bool SessionCoordinator::IsSessionIdAllocated(uint16_t session_id) const
        {
            if (session_id >= MAX_SESSION_IDS)
            {
                return false;
            }

            std::lock_guard<std::mutex> lock(allocated_ids_mutex_);
            return allocated_ids_[session_id];
        }

        uint16_t SessionCoordinator::FindAvailableSessionId()
        {
            // 从 next_session_id_ 开始搜索
            uint16_t start_id = next_session_id_.load();
            uint16_t current_id = start_id;

            do
            {
                // 检查当前 session_id 是否可用
                if (current_id < MAX_SESSION_IDS && !allocated_ids_[current_id])
                {
                    // 检查是否为保留的 session_id
                    bool is_reserved = false;
                    for (uint16_t reserved_id : reserved_session_ids_)
                    {
                        if (current_id == reserved_id)
                        {
                            is_reserved = true;
                            break;
                        }
                    }

                    if (!is_reserved)
                    {
                        // 更新 next_session_id_ 为下一个可能的 ID
                        next_session_id_.store(
                            (current_id + 1) % (MAX_SESSION_IDS - 1));
                        if (next_session_id_.load() <= 1) // 避免使用保留的 ID
                        {
                            next_session_id_.store(2);
                        }
                        return current_id;
                    }
                }

                // 移动到下一个 session_id
                current_id++;
                if (current_id >= MAX_SESSION_IDS)
                {
                    current_id = 2; // 从 2 重新开始
                }

                // 如果回到了起始点，说明没有可用的 session_id
                if (current_id == start_id)
                {
                    break;
                }

            } while (true);

            return 0; // 没有可用的 session_id
        }

        void SessionCoordinator::MarkSessionIdAsAllocated(uint16_t session_id)
        {
            if (session_id < MAX_SESSION_IDS)
            {
                allocated_ids_[session_id] = true;
            }
        }

        void SessionCoordinator::MarkSessionIdAsFree(uint16_t session_id)
        {
            if (session_id < MAX_SESSION_IDS)
            {
                allocated_ids_[session_id] = false;
            }
        }
    }
}
DAS_NS_END