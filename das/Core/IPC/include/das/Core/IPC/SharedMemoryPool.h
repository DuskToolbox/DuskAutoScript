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
 * - 如果 SharedMemoryPool 被 Uninitialize/销毁，所有块将自动失效
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

/**
 * @brief 共享内存池创建模式
 *
 * 用于区分创建新池和打开已存在池两种操作
 */
enum class PoolMode
{
    Create, ///< 创建新共享内存池
    Open    ///< 打开已存在的共享内存池
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
 * - Initialize() 创建共享内存段；Uninitialize() 销毁它
 * - 析构函数自动调用 Uninitialize()（RAII）
 * - Uninitialize() 是幂等的：多次调用安全，仅首次生效
 *
 * 线程安全：所有公共方法都是线程安全的
 *
 * RAII 模式：使用 Create() 工厂函数创建，析构自动清理
 */
class SharedMemoryPool
{
public:
    /**
     * @brief RAII 构造函数：创建或打开共享内存池
     * @param pool_name 共享内存池名称
     * @param initial_size 初始大小（仅 Create 模式使用）
     * @param mode 创建模式：Create 或 Open
     * @throws std::runtime_error 如果共享内存操作失败
     */
    explicit SharedMemoryPool(
        const std::string& pool_name,
        size_t             initial_size,
        PoolMode           mode = PoolMode::Create);

    /**
     * @brief 打开已存在的共享内存池（用于远端进程）
     * @param pool_name 共享内存池名称
     * @return std::unique_ptr<SharedMemoryPool> SharedMemoryPool 智能指针
     * @throws std::runtime_error 如果共享内存打开失败
     */
    static std::unique_ptr<SharedMemoryPool> Open(const std::string& pool_name);

    ~SharedMemoryPool();

    DasResult Allocate(size_t size, SharedMemoryBlock& block);
    DasResult Deallocate(uint64_t handle);
    DasResult GetBlockByHandle(uint64_t handle, SharedMemoryBlock& block);

    DasResult CleanupStaleBlocks();

    size_t GetTotalSize() const;
    size_t GetUsedSize() const;

private:
    void Uninitialize();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class SharedMemoryManager
{
public:
    /// @brief 公开构造函数
    SharedMemoryManager();

    ~SharedMemoryManager();

    DasResult CreatePool(const std::string& pool_id, size_t size);
    DasResult DestroyPool(const std::string& pool_id);

    DasResult GetPool(const std::string& pool_id, SharedMemoryPool*& pool);

    static std::string MakePoolName(uint16_t host_id, uint16_t pool_id);

private:
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
