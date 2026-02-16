#ifndef DAS_CORE_IPC_SESSION_COORDINATOR_H
#define DAS_CORE_IPC_SESSION_COORDINATOR_H

#include <atomic>
#include <cstdint>
#include <das/IDasBase.h>
#include <mutex>
#include <vector>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        /**
         * @brief Session ID 管理器
         *
         * 统一管理 session_id 的分配和释放，避免多进程 ID 冲突
         *
         * Session ID 分配规则：
         * - session_id = 0: 保留
         * - session_id = 1: 主进程
         * - session_id = 2~0xFFFE: Host 进程
         * - session_id = 0xFFFF: 保留
         */
        class SessionCoordinator
        {
        public:
            /**
             * @brief 获取 SessionCoordinator 单例实例
             */
            static SessionCoordinator& GetInstance();

            /**
             * @brief 分配新的 session_id
             * @return 分配的 session_id，如果失败返回 0
             */
            uint16_t AllocateSessionId();

            /**
             * @brief 释放 session_id
             * @param session_id 要释放的 session_id
             */
            void ReleaseSessionId(uint16_t session_id);

            /**
             * @brief 获取当前进程的本地 session_id
             * @return 本地 session_id，如果未设置返回 0
             */
            uint16_t GetLocalSessionId() const;

            /**
             * @brief 设置当前进程的本地 session_id
             * @param session_id 要设置的本地 session_id
             */
            void SetLocalSessionId(uint16_t session_id);

            /**
             * @brief 检查 session_id 是否有效
             * @param session_id 要检查的 session_id
             * @return true 如果 session_id 有效，false 否则
             */
            static bool IsValidSessionId(uint16_t session_id);

            /**
             * @brief 检查 session_id 是否已分配
             * @param session_id 要检查的 session_id
             * @return true 如果 session_id 已分配，false 否则
             */
            bool IsSessionIdAllocated(uint16_t session_id) const;

            /**
             * @brief 禁用拷贝构造
             */
            SessionCoordinator(const SessionCoordinator&) = delete;

            /**
             * @brief 禁用赋值操作
             */
            SessionCoordinator& operator=(const SessionCoordinator&) = delete;

        private:
            /**
             * @brief 构造函数（私有，单例模式）
             */
            SessionCoordinator();

            /**
             * @brief 析构函数
             */
            ~SessionCoordinator();

            /**
             * @brief 查找下一个可用的 session_id
             * @return 可用的 session_id，如果没有返回 0
             */
            uint16_t FindAvailableSessionId();

            /**
             * @brief 标记 session_id 为已分配
             * @param session_id 要标记的 session_id
             */
            void MarkSessionIdAsAllocated(uint16_t session_id);

            /**
             * @brief 标记 session_id 为未分配
             * @param session_id 要标记的 session_id
             */
            void MarkSessionIdAsFree(uint16_t session_id);

            // 成员变量
            std::atomic<uint16_t> next_session_id_{
                2}; // 下一个可分配的 session_id（从 2 开始）
            std::atomic<uint16_t> local_session_id_{
                0}; // 当前进程的本地 session_id

            // 使用位图来跟踪已分配的 session_id（最多 65536 个位）
            static const size_t MAX_SESSION_IDS = 65536;
            mutable std::mutex  allocated_ids_mutex_;
            bool                allocated_ids_[MAX_SESSION_IDS]{};

            // 标记保留的 session_id
            static const uint16_t reserved_session_ids_[3];
        };
    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_SESSION_COORDINATOR_H