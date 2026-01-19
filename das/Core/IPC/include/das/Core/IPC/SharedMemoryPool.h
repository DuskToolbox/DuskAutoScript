#ifndef DAS_CORE_IPC_SHARED_MEMORY_POOL_H
#define DAS_CORE_IPC_SHARED_MEMORY_POOL_H

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/IDasBase.h>
#include <memory>
#include <string>
#include <unordered_map>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        struct SharedMemoryBlock
        {
            void*       data;
            size_t      size;
            std::string name;
        };

        class SharedMemoryPool
        {
        public:
            SharedMemoryPool();
            ~SharedMemoryPool();

            DasResult Initialize(
                const std::string& pool_name,
                size_t             initial_size);
            DasResult Shutdown();

            DasResult Allocate(size_t size, SharedMemoryBlock& block);
            DasResult Deallocate(const std::string& block_name);

            DasResult CleanupStaleBlocks();

            size_t GetTotalSize() const;
            size_t GetUsedSize() const;

        private:
            struct Impl;
            std::unique_ptr<Impl> impl_;
        };

        class SharedMemoryManager
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
    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_SHARED_MEMORY_POOL_H
