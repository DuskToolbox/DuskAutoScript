#ifndef DAS_CORE_IPC_SHARED_MEMORY_POOL_H
#define DAS_CORE_IPC_SHARED_MEMORY_POOL_H

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/DasExport.h>
#include <das/IDasBase.h>
#include <memory>
#include <string>
#include <unordered_map>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        /**
         * @brief 共享内存块描述结构
         *
         * 所有权语义：
         * - Allocate() 返回的 SharedMemoryBlock 的所有权属于调用者
         * - 调用者负责在不再使用时调用 Deallocate() 释放内存
         * - 如果 SharedMemoryPool 被 Shutdown/销毁，所有块将自动失效
         * - block.handle 是跨进程稳定的偏移量，用于定位块
         *
         * 跨进程访问（B3 规范）：
         * - 发送方：通过 Allocate 获取 handle，发送给接收方
         * - 接收方：通过 handle 访问同一共享内存池中的数据
         * - handle 是相对于共享内存段起始位置的偏移量，跨进程稳定
         */
        struct SharedMemoryBlock
        {
            void*    data;   ///< 内存块数据指针（本进程内的地址）
            size_t   size;   ///< 内存块大小（字节）
            uint64_t handle; ///< 跨进程稳定的偏移量（用于 IPC 传递）
        };

// Disable C4251 warning for std::unique_ptr in exported classes
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

        /**
         * @brief 共享内存池管理器
         *
         * 生命周期与所有权：
         * - Initialize() 创建共享内存段；Shutdown() 销毁它
         * - 析构函数自动调用 Shutdown()（RAII）
         * - Shutdown() 是幂等的：多次调用安全，仅首次生效
         *
         * 线程安全：所有公共方法都是线程安全的
         */
        class DAS_API SharedMemoryPool
        {
        public:
            SharedMemoryPool();
            ~SharedMemoryPool();

            DasResult Initialize(
                const std::string& pool_name,
                size_t             initial_size);
            DasResult Shutdown();

            DasResult Allocate(size_t size, SharedMemoryBlock& block);
            DasResult Deallocate(uint64_t handle);
            DasResult GetBlockByHandle(
                uint64_t           handle,
                SharedMemoryBlock& block);

            DasResult CleanupStaleBlocks();

            size_t GetTotalSize() const;
            size_t GetUsedSize() const;

        private:
            struct Impl;
            std::unique_ptr<Impl> impl_;
        };

        class DAS_API SharedMemoryManager
        {
        public:
            SharedMemoryManager();
            ~SharedMemoryManager();

            DasResult Initialize();
            DasResult Shutdown();

            DasResult CreatePool(const std::string& pool_id, size_t size);
            DasResult DestroyPool(const std::string& pool_id);

            DasResult GetPool(
                const std::string& pool_id,
                SharedMemoryPool*& pool);

            static std::string MakePoolName(uint16_t host_id, uint16_t pool_id);

        private:
            struct Impl;
            std::unique_ptr<Impl> impl_;
        };

#ifdef _MSC_VER
#pragma warning(pop)
#endif

    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_SHARED_MEMORY_POOL_H
