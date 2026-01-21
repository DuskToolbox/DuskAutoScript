#include <cstdint>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <mutex>
#include <string>
#include <unordered_map>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        struct SharedMemoryPool::Impl
        {
            std::unique_ptr<boost::interprocess::managed_shared_memory>
                               segment_;
            std::string        name_;
            size_t             total_size_;
            size_t             used_size_{0};
            mutable std::mutex mutex_;
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
            catch (const boost::interprocess::interprocess_exception& ex)
            {
                return DAS_E_IPC_SHM_FAILED;
            }
        }

        DasResult SharedMemoryPool::Shutdown()
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);

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
                    return DAS_E_OUTOFMEMORY;
                }

                std::string block_name =
                    "block_" + std::to_string(reinterpret_cast<uintptr_t>(ptr));

                block.data = ptr;
                block.size = size;
                block.name = block_name;

                impl_->used_size_ += size;
                return DAS_S_OK;
            }
            catch (const boost::interprocess::interprocess_exception& ex)
            {
                return DAS_E_IPC_SHM_FAILED;
            }
        }

        DasResult SharedMemoryPool::Deallocate(const std::string& block_name)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);

            if (!impl_->segment_)
            {
                return DAS_E_IPC_SHM_FAILED;
            }

            try
            {
                auto* ptr =
                    impl_->segment_->find<void>(block_name.c_str()).first;
                if (ptr)
                {
                    impl_->segment_->deallocate(ptr);

                    size_t dealloc_size = 0;
                    impl_->segment_->deallocate(ptr);
                    impl_->used_size_ -= dealloc_size;
                }

                return DAS_S_OK;
            }
            catch (const boost::interprocess::interprocess_exception& ex)
            {
                return DAS_E_IPC_SHM_FAILED;
            }
        }

        DasResult SharedMemoryPool::CleanupStaleBlocks() { return DAS_S_OK; }

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
            std::string pool_name = MakePoolName(1, std::stoul(pool_id));

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

            it->second->Shutdown();
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
