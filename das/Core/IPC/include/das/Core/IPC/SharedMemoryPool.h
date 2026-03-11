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

#include <das/Core/IPC/Config.h>
#include <das/DasConfig.h>

DAS_CORE_IPC_NS_BEGIN
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
 *
 * RAII 模式：使用 Create() 工厂函数创建，析构自动清理
 */
class SharedMemoryPool
{
public:
    /**
     * @brief 工厂函数：创建并初始化 SharedMemoryPool 实例
     * @param pool_name 共享内存池名称
     * @param initial_size 初始大小
     * @return std::unique_ptr<SharedMemoryPool> SharedMemoryPool 智能指针
     */
    static std::unique_ptr<SharedMemoryPool> Create(
        const std::string& pool_name,
        size_t             initial_size);

    ~SharedMemoryPool();

    DasResult Shutdown();

    DasResult Allocate(size_t size, SharedMemoryBlock& block);
    DasResult Deallocate(uint64_t handle);
    DasResult GetBlockByHandle(uint64_t handle, SharedMemoryBlock& block);

    DasResult CleanupStaleBlocks();

    size_t GetTotalSize() const;
    size_t GetUsedSize() const;

private:
    // 私有构造函数 - 只能通过 Create() 工厂函数调用
    SharedMemoryPool();
    friend class std::unique_ptr<SharedMemoryPool>;

    // 私有初始化函数 - 只能由 Create() 工厂函数调用
    DasResult Initialize(const std::string& pool_name, size_t initial_size);

    // 私有清理函数 - 只能由析构函数调用
    void Uninitialize();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class SharedMemoryManager
{
public:
    /**
     * @brief 工厂函数：创建 SharedMemoryManager 实例
     * @return std::unique_ptr<SharedMemoryManager> SharedMemoryManager 智能指针
     */
    static std::unique_ptr<SharedMemoryManager> Create();

    ~SharedMemoryManager();

    DasResult Shutdown();

    // DasResult Initialize();

    DasResult CreatePool(const std::string& pool_id, size_t size);
    DasResult DestroyPool(const std::string& pool_id);

    DasResult GetPool(const std::string& pool_id, SharedMemoryPool*& pool);

    static std::string MakePoolName(uint16_t host_id, uint16_t pool_id);

private:
    // 私有构造函数 - 只能通过 Create() 工厂函数调用
    SharedMemoryManager();
    friend class std::unique_ptr<SharedMemoryManager>;

    // 私有初始化函数 - 只能由 Create() 工厂函数调用
    DasResult Initialize();

    // 私有清理函数 - 只能由析构函数调用
    void Uninitialize();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_SHARED_MEMORY_POOL_H
