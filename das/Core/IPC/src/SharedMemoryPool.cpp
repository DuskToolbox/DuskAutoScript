#include <boost/interprocess/managed_shared_memory.hpp>
#include <chrono>
#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcPermissions.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/Logger/Logger.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

DAS_CORE_IPC_NS_BEGIN
/**
 * @brief 共享内存块元数据
 *
 * 用于跟踪块的分配时间，支持 CleanupStaleBlocks 功能。
 * 超过 STALE_THRESHOLD 的块会被自动清理，防止泄漏。
 */
struct BlockMetadata
{
    size_t size; ///< 块大小（字节）
    std::chrono::steady_clock::time_point
        allocation_time; ///< 分配时间（用于超时清理判断）
};

struct SharedMemoryPool::Impl
{
    std::unique_ptr<boost::interprocess::managed_shared_memory> segment_;
    std::string                                                 name_;
    size_t                                                      total_size_;
    size_t                                                      used_size_{0};
    std::unordered_map<uint64_t, BlockMetadata>                 block_metadata_;
    mutable std::mutex                                          mutex_;

    static constexpr std::chrono::seconds STALE_THRESHOLD{60};
};

SharedMemoryPool::SharedMemoryPool(
    const std::string& pool_name,
    size_t             initial_size,
    PoolMode           mode)
    : impl_(std::make_unique<Impl>())
{
    impl_->name_ = pool_name;

    if (mode == PoolMode::Create)
    {
        impl_->total_size_ = initial_size;

        boost::interprocess::shared_memory_object::remove(pool_name.c_str());

        const IpcSecurityAttributes ipc_sec;
        impl_->segment_ =
            std::make_unique<boost::interprocess::managed_shared_memory>(
                boost::interprocess::create_only,
                pool_name.c_str(),
                initial_size,
                nullptr, // addr
                ipc_sec.GetPermissions());
    }
    else
    {
        impl_->segment_ =
            std::make_unique<boost::interprocess::managed_shared_memory>(
                boost::interprocess::open_only,
                pool_name.c_str());
        impl_->total_size_ = impl_->segment_->get_size();
    }
}

std::unique_ptr<SharedMemoryPool> SharedMemoryPool::Open(
    const std::string& pool_name)
{
    try
    {
        return std::make_unique<SharedMemoryPool>(pool_name, 0, PoolMode::Open);
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_EXCEPTION(e);
        throw std::runtime_error(
            std::string("Failed to open shared memory pool: ") + e.what());
    }
}

SharedMemoryPool::~SharedMemoryPool() { Uninitialize(); }

void SharedMemoryPool::Uninitialize()
{
    const std::scoped_lock lock(impl_->mutex_);

    // 幂等性检查：如果已经关闭，直接返回
    // 这防止了 DestroyPool 中显式调用 Uninitialize() 后，
    // 析构函数再次调用 Uninitialize() 导致的重复操作
    if (!impl_->segment_)
    {
        return;
    }

    impl_->segment_.reset();

    try
    {
        boost::interprocess::shared_memory_object::remove(impl_->name_.c_str());
    }
    catch (...)
    {
    }

    impl_->used_size_ = 0;
    impl_->block_metadata_.clear();
}

DasResult SharedMemoryPool::Allocate(size_t size, SharedMemoryBlock& block)
{
    const std::scoped_lock lock(impl_->mutex_);

    if (!impl_->segment_)
    {
        DAS_CORE_LOG_ERROR("Shared memory not initialized");
        return DAS_E_IPC_SHM_FAILED;
    }

    try
    {
        void* ptr = impl_->segment_->allocate(size);
        if (!ptr)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to allocate {} bytes (null pointer returned)",
                size);
            return DAS_E_OUT_OF_MEMORY;
        }

        auto handle = impl_->segment_->get_handle_from_address(ptr);

        block.data = ptr;
        block.size = size;
        block.handle = static_cast<uint64_t>(handle);

        impl_->block_metadata_[block.handle] =
            BlockMetadata{size, std::chrono::steady_clock::now()};
        impl_->used_size_ += size;
        return DAS_S_OK;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_EXCEPTION(e);
        return DAS_E_IPC_SHM_FAILED;
    }
}

DasResult SharedMemoryPool::GetAddressByHandle(uint64_t handle, void*& out)
{
    if (!impl_->segment_)
    {
        DAS_CORE_LOG_ERROR("Shared memory not initialized");
        return DAS_E_IPC_SHM_FAILED;
    }

    try
    {
        auto managed_handle =
            static_cast<boost::interprocess::managed_shared_memory::handle_t>(
                handle);
        out = impl_->segment_->get_address_from_handle(managed_handle);
        return DAS_S_OK;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_EXCEPTION(e);
        return DAS_E_IPC_SHM_FAILED;
    }
}

DasResult SharedMemoryPool::Deallocate(uint64_t handle)
{
    const std::scoped_lock lock(impl_->mutex_);

    if (!impl_->segment_)
    {
        DAS_CORE_LOG_ERROR("Shared memory not initialized");
        return DAS_E_IPC_SHM_FAILED;
    }

    try
    {
        auto managed_handle =
            static_cast<boost::interprocess::managed_shared_memory::handle_t>(
                handle);
        void* ptr = impl_->segment_->get_address_from_handle(managed_handle);

        // 段范围 + 对齐校验：拒绝越界或未对齐的 handle，避免 boost 内部
        // deallocate 解引用段外非法指针触发 ASAN access-violation。
        // 不依赖本地 block_metadata_：跨进程场景下接收方/远端 Deallocate
        // 对端分配的 handle 时本地无 metadata，但共享段是同一物理段，
        // 范围校验对合法跨进程 handle 仍然成立。
        char*  seg_base = static_cast<char*>(impl_->segment_->get_address());
        size_t seg_size = impl_->segment_->get_size();
        if (seg_base == nullptr || seg_size == 0)
        {
            DAS_CORE_LOG_ERROR(
                "Deallocate: invalid segment state (base={}, size={})",
                static_cast<void*>(seg_base),
                seg_size);
            return DAS_E_IPC_SHM_FAILED;
        }

        const auto ptr_val = reinterpret_cast<uintptr_t>(ptr);
        const auto base_val = reinterpret_cast<uintptr_t>(seg_base);
        if (ptr_val < base_val || ptr_val >= base_val + seg_size)
        {
            DAS_CORE_LOG_WARN(
                "Deallocate: handle {} out of segment range (base={}, size={})",
                handle,
                base_val,
                seg_size);
            return DAS_E_IPC_OBJECT_NOT_FOUND;
        }

        // boost managed_shared_memory 的内部分配按至少 sizeof(void*) 对齐
        constexpr uintptr_t min_alignment = sizeof(void*);
        if (ptr_val % min_alignment != 0)
        {
            DAS_CORE_LOG_WARN(
                "Deallocate: handle {} misaligned (ptr={})",
                handle,
                ptr_val);
            return DAS_E_IPC_OBJECT_NOT_FOUND;
        }

        impl_->segment_->deallocate(ptr);

        // Best-effort: 尝试从 metadata 中移除并回扣 used_size
        // 跨进程场景下，接收方的进程没有此 metadata，跳过此步不报错
        auto it = impl_->block_metadata_.find(handle);
        if (it != impl_->block_metadata_.end())
        {
            impl_->used_size_ -= it->second.size;
            impl_->block_metadata_.erase(it);
        }

        return DAS_S_OK;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_EXCEPTION(e);
        return DAS_E_IPC_SHM_FAILED;
    }
}

DasResult SharedMemoryPool::GetBlockByHandle(
    uint64_t           handle,
    SharedMemoryBlock& block)
{
    const std::scoped_lock lock(impl_->mutex_);

    if (!impl_->segment_)
    {
        DAS_CORE_LOG_ERROR("Shared memory not initialized");
        return DAS_E_IPC_SHM_FAILED;
    }

    auto it = impl_->block_metadata_.find(handle);
    if (it == impl_->block_metadata_.end())
    {
        DAS_CORE_LOG_ERROR("Block not found for handle = {}", handle);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    try
    {
        auto managed_handle =
            static_cast<boost::interprocess::managed_shared_memory::handle_t>(
                handle);
        void* ptr = impl_->segment_->get_address_from_handle(managed_handle);

        block.data = ptr;
        block.size = it->second.size;
        block.handle = handle;

        return DAS_S_OK;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_EXCEPTION(e);
        return DAS_E_IPC_SHM_FAILED;
    }
}

DasResult SharedMemoryPool::CleanupStaleBlocks()
{
    // 注意：跨进程场景下此函数仅能清理"本进程 metadata 记录的块"。
    // 真正的大消息块由接收方通过 ShmBlockGuard 析构释放。
    // 崩溃兜底：随连接销毁 + 进程重启时 shared_memory_object::remove（见 Create
    // 分支）。
    const std::scoped_lock lock(impl_->mutex_);

    if (!impl_->segment_)
    {
        DAS_CORE_LOG_ERROR("Shared memory not initialized");
        return DAS_E_IPC_SHM_FAILED;
    }

    auto                  now = std::chrono::steady_clock::now();
    std::vector<uint64_t> blocks_to_remove;

    for (const auto& [handle, metadata] : impl_->block_metadata_)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - metadata.allocation_time);
        if (elapsed >= Impl::STALE_THRESHOLD)
        {
            blocks_to_remove.push_back(handle);
        }
    }

    for (const uint64_t handle : blocks_to_remove)
    {
        auto it = impl_->block_metadata_.find(handle);
        if (it != impl_->block_metadata_.end())
        {
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
            }
            catch (...)
            {
            }
        }
    }

    return DAS_S_OK;
}

size_t SharedMemoryPool::GetTotalSize() const { return impl_->total_size_; }

size_t SharedMemoryPool::GetUsedSize() const
{
    const std::scoped_lock lock(impl_->mutex_);
    return impl_->used_size_;
}

struct SharedMemoryManager::Impl
{
    std::unordered_map<std::string, std::unique_ptr<SharedMemoryPool>> pools_;
    mutable std::mutex                                                 mutex_;
};

SharedMemoryManager::SharedMemoryManager() : impl_(std::make_unique<Impl>()) {}

SharedMemoryManager::~SharedMemoryManager() { Uninitialize(); }

void SharedMemoryManager::Uninitialize()
{
    const std::scoped_lock lock(impl_->mutex_);
    impl_->pools_.clear();
}

DasResult SharedMemoryManager::CreatePool(
    const std::string& pool_id,
    size_t             size)
{
    const std::string pool_name =
        MakePoolName(1, static_cast<uint16_t>(std::stoul(pool_id)));

    auto pool = std::make_unique<SharedMemoryPool>(pool_name, size);

    const std::scoped_lock lock(impl_->mutex_);
    impl_->pools_[pool_id] = std::move(pool);
    return DAS_S_OK;
}

DasResult SharedMemoryManager::DestroyPool(const std::string& pool_id)
{
    const std::scoped_lock lock(impl_->mutex_);
    auto                   it = impl_->pools_.find(pool_id);
    if (it == impl_->pools_.end())
    {
        DAS_CORE_LOG_ERROR("Pool not found for pool_id = {}", pool_id);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    // 直接 erase 即可，SharedMemoryPool 析构函数会自动调用 Uninitialize()
    // 无需显式调用 Uninitialize()，避免重复调用
    impl_->pools_.erase(it);
    return DAS_S_OK;
}

DasResult SharedMemoryManager::GetPool(
    const std::string& pool_id,
    SharedMemoryPool*& pool)
{
    const std::scoped_lock lock(impl_->mutex_);
    auto                   it = impl_->pools_.find(pool_id);
    if (it == impl_->pools_.end())
    {
        DAS_CORE_LOG_ERROR("Pool not found for pool_id = {}", pool_id);
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    pool = it->second.get();
    return DAS_S_OK;
}

std::string SharedMemoryManager::MakePoolName(
    uint16_t host_id,
    uint16_t pool_id)
{
    return "das_shm_" + std::to_string(host_id) + "_" + std::to_string(pool_id);
}

// ============================================================================
// ShmBlockGuard Implementation
// ============================================================================

ShmBlockGuard::~ShmBlockGuard() { Reset(); }

ShmBlockGuard::ShmBlockGuard(ShmBlockGuard&& other) noexcept
    : pool_(other.pool_), data_(other.data_), size_(other.size_),
      handle_(other.handle_)
{
    other.pool_ = nullptr;
    other.data_ = nullptr;
    other.size_ = 0;
    other.handle_ = 0;
}

ShmBlockGuard& ShmBlockGuard::operator=(ShmBlockGuard&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        pool_ = other.pool_;
        data_ = other.data_;
        size_ = other.size_;
        handle_ = other.handle_;
        other.pool_ = nullptr;
        other.data_ = nullptr;
        other.size_ = 0;
        other.handle_ = 0;
    }
    return *this;
}

ShmBlockGuard ShmBlockGuard::Attach(
    SharedMemoryPool* pool,
    uint64_t          handle,
    size_t            size)
{
    if (!pool)
    {
        return ShmBlockGuard();
    }

    void* data = nullptr;
    auto  hr = pool->GetAddressByHandle(handle, data);
    if (DAS::IsFailed(hr) || !data)
    {
        return ShmBlockGuard();
    }

    return ShmBlockGuard(pool, data, size, handle);
}

SharedMemoryBlock ShmBlockGuard::Disown() noexcept
{
    SharedMemoryBlock block{data_, size_, handle_};
    pool_ = nullptr;
    data_ = nullptr;
    size_ = 0;
    handle_ = 0;
    return block;
}

void ShmBlockGuard::Reset() noexcept
{
    if (pool_ && handle_)
    {
        [[maybe_unused]]
        auto hr = pool_->Deallocate(handle_);
        (void)hr; // Explicitly ignore result in release builds
    }
    pool_ = nullptr;
    data_ = nullptr;
    size_ = 0;
    handle_ = 0;
}

DAS_CORE_IPC_NS_END
