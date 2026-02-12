#include <boost/interprocess/managed_shared_memory.hpp>
#include <chrono>
#include <cstdint>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        /**
         * @brief 共享内存块元数据
         *
         *
         * 用于跟踪块的引用计数和分配时间，支持 CleanupStaleBlocks
         * 功能

         */
        struct BlockMetadata
        {
            size_t   size;      ///< 块大小（字节）
            uint32_t ref_count; ///< 引用计数（0 表示无活跃引用）
            std::chrono::steady_clock::time_point
                allocation_time; ///< 分配时间（用于超时清理判断）
        };

        struct SharedMemoryPool::Impl
        {
            std::unique_ptr<boost::interprocess::managed_shared_memory>
                                                        segment_;
            std::string                                 name_;
            size_t                                      total_size_;
            size_t                                      used_size_{0};
            std::unordered_map<uint64_t, BlockMetadata> block_metadata_;
            mutable std::mutex                          mutex_;

            static constexpr std::chrono::seconds kStaleThreshold{60};
        };

        SharedMemoryPool::SharedMemoryPool() : impl_(std::make_unique<Impl>())
        {
        }

        SharedMemoryPool::~SharedMemoryPool() { Shutdown(); }

        DasResult SharedMemoryPool::Initialize(
            const std::string& pool_name,
            size_t             initial_size)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);

            impl_->name_ = pool_name;
            impl_->total_size_ = initial_size;

            try
            {
                boost::interprocess::shared_memory_object::remove(
                    pool_name.c_str());

                impl_->segment_ = std::make_unique<
                    boost::interprocess::managed_shared_memory>(
                    boost::interprocess::create_only,
                    pool_name.c_str(),
                    initial_size);

                return DAS_S_OK;
            }
            catch (...)
            {
                return DAS_E_IPC_SHM_FAILED;
            }
        }

        DasResult SharedMemoryPool::Shutdown()
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);

            // 幂等性检查：如果已经关闭，直接返回成功
            // 这防止了 DestroyPool 中显式调用 Shutdown() 后，
            // 析构函数再次调用 Shutdown() 导致的重复操作
            if (!impl_->segment_)
            {
                return DAS_S_OK;
            }

            impl_->segment_.reset();

            try
            {
                boost::interprocess::shared_memory_object::remove(
                    impl_->name_.c_str());
            }
            catch (...)
            {
            }

            impl_->used_size_ = 0;
            impl_->block_metadata_.clear();
            return DAS_S_OK;
        }

        DasResult SharedMemoryPool::Allocate(
            size_t             size,
            SharedMemoryBlock& block)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);

            if (!impl_->segment_)
            {
                return DAS_E_IPC_SHM_FAILED;
            }

            try
            {
                void* ptr = impl_->segment_->allocate(size);
                if (!ptr)
                {
                    return DAS_E_OUT_OF_MEMORY;
                }

                auto handle = impl_->segment_->get_handle_from_address(ptr);

                block.data = ptr;
                block.size = size;
                block.handle = static_cast<uint64_t>(handle);

                impl_->block_metadata_[block.handle] =
                    BlockMetadata{size, 1, std::chrono::steady_clock::now()};
                impl_->used_size_ += size;
                return DAS_S_OK;
            }
            catch (...)
            {
                return DAS_E_IPC_SHM_FAILED;
            }
        }

        DasResult SharedMemoryPool::Deallocate(uint64_t handle)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);

            if (!impl_->segment_)
            {
                return DAS_E_IPC_SHM_FAILED;
            }

            auto it = impl_->block_metadata_.find(handle);
            if (it == impl_->block_metadata_.end())
            {
                return DAS_E_IPC_SHM_FAILED;
            }

            try
            {
                auto managed_handle = static_cast<
                    boost::interprocess::managed_shared_memory::handle_t>(
                    handle);
                void* ptr =
                    impl_->segment_->get_address_from_handle(managed_handle);

                impl_->segment_->deallocate(ptr);

                impl_->used_size_ -= it->second.size;
                impl_->block_metadata_.erase(it);

                return DAS_S_OK;
            }
            catch (...)
            {
                return DAS_E_IPC_SHM_FAILED;
            }
        }

        DasResult SharedMemoryPool::GetBlockByHandle(
            uint64_t           handle,
            SharedMemoryBlock& block)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);

            if (!impl_->segment_)
            {
                return DAS_E_IPC_SHM_FAILED;
            }

            auto it = impl_->block_metadata_.find(handle);
            if (it == impl_->block_metadata_.end())
            {
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            try
            {
                auto managed_handle = static_cast<
                    boost::interprocess::managed_shared_memory::handle_t>(
                    handle);
                void* ptr =
                    impl_->segment_->get_address_from_handle(managed_handle);

                block.data = ptr;
                block.size = it->second.size;
                block.handle = handle;

                return DAS_S_OK;
            }
            catch (...)
            {
                return DAS_E_IPC_SHM_FAILED;
            }
        }

        DasResult SharedMemoryPool::CleanupStaleBlocks()
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);

            if (!impl_->segment_)
            {
                return DAS_E_IPC_SHM_FAILED;
            }

            auto                  now = std::chrono::steady_clock::now();
            std::vector<uint64_t> blocks_to_remove;

            for (const auto& [handle, metadata] : impl_->block_metadata_)
            {
                if (metadata.ref_count == 0)
                {
                    auto elapsed =
                        std::chrono::duration_cast<std::chrono::seconds>(
                            now - metadata.allocation_time);
                    if (elapsed >= Impl::kStaleThreshold)
                    {
                        blocks_to_remove.push_back(handle);
                    }
                }
            }

            for (uint64_t handle : blocks_to_remove)
            {
                auto it = impl_->block_metadata_.find(handle);
                if (it != impl_->block_metadata_.end())
                {
                    try
                    {
                        auto managed_handle =
                            static_cast<boost::interprocess::
                                            managed_shared_memory::handle_t>(
                                handle);
                        void* ptr = impl_->segment_->get_address_from_handle(
                            managed_handle);
                        impl_->segment_->deallocate(ptr);
                        impl_->used_size_ -= it->second.size;
                        impl_->block_metadata_.erase(it);
                    }
                    catch (...)
                    {
                    }
                }
            }

            return DAS_S_OK;
        }

        size_t SharedMemoryPool::GetTotalSize() const
        {
            return impl_->total_size_;
        }

        size_t SharedMemoryPool::GetUsedSize() const
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);
            return impl_->used_size_;
        }

        struct SharedMemoryManager::Impl
        {
            std::unordered_map<std::string, std::unique_ptr<SharedMemoryPool>>
                               pools_;
            mutable std::mutex mutex_;
        };

        SharedMemoryManager::SharedMemoryManager()
            : impl_(std::make_unique<Impl>())
        {
        }

        SharedMemoryManager::~SharedMemoryManager() { Shutdown(); }

        DasResult SharedMemoryManager::Initialize() { return DAS_S_OK; }

        DasResult SharedMemoryManager::Shutdown()
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);
            impl_->pools_.clear();
            return DAS_S_OK;
        }

        DasResult SharedMemoryManager::CreatePool(
            const std::string& pool_id,
            size_t             size)
        {
            std::string pool_name =
                MakePoolName(1, static_cast<uint16_t>(std::stoul(pool_id)));

            auto pool = std::make_unique<SharedMemoryPool>();
            auto result = pool->Initialize(pool_name, size);
            if (result != DAS_S_OK)
            {
                return result;
            }

            std::lock_guard<std::mutex> lock(impl_->mutex_);
            impl_->pools_[pool_id] = std::move(pool);
            return DAS_S_OK;
        }

        DasResult SharedMemoryManager::DestroyPool(const std::string& pool_id)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);
            auto                        it = impl_->pools_.find(pool_id);
            if (it == impl_->pools_.end())
            {
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            // 直接 erase 即可，SharedMemoryPool 析构函数会自动调用 Shutdown()
            // 无需显式调用 Shutdown()，避免重复调用
            impl_->pools_.erase(it);
            return DAS_S_OK;
        }

        DasResult SharedMemoryManager::GetPool(
            const std::string& pool_id,
            SharedMemoryPool*& pool)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);
            auto                        it = impl_->pools_.find(pool_id);
            if (it == impl_->pools_.end())
            {
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            pool = it->second.get();
            return DAS_S_OK;
        }

        std::string SharedMemoryManager::MakePoolName(
            uint16_t host_id,
            uint16_t pool_id)
        {
            return "das_shm_" + std::to_string(host_id) + "_"
                   + std::to_string(pool_id);
        }
    }
}
DAS_NS_END
