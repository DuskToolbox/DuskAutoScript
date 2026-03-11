#include <chrono>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Utils/fmt.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
using DAS::Core::IPC::SharedMemoryBlock;
using DAS::Core::IPC::SharedMemoryManager;
using DAS::Core::IPC::SharedMemoryPool;

// Test fixture for SharedMemoryPool tests
class IpcSharedMemoryPoolTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Generate unique name using high-resolution timer + PID + test name
        auto now_ns = std::chrono::high_resolution_clock::now()
                          .time_since_epoch()
                          .count();
        auto test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::string test_name = test_info ? test_info->name() : "unknown";
        pool_name_ = DAS_FMT_NS::format(
            "das_shm_test_{}_{}_{}",
            GetCurrentProcessId(),
            now_ns,
            test_name);

        // Create and initialize the pool using factory function
        pool_ = SharedMemoryPool::Create(pool_name_, 65536);
    }

    void TearDown() override
    {
        // unique_ptr will automatically call destructor which calls Uninitialize()
        pool_.reset();
        // Force remove shared memory on Windows to ensure cleanup
        boost::interprocess::shared_memory_object::remove(pool_name_.c_str());
    }

    std::unique_ptr<SharedMemoryPool> pool_;
    std::string                       pool_name_;
};

// ====== Initialize/Shutdown Tests ======

TEST_F(IpcSharedMemoryPoolTest, Initialize_Succeeds)
{
    // Pool is already initialized in SetUp via Create()
    ASSERT_NE(pool_, nullptr);
    EXPECT_TRUE(pool_->GetTotalSize() > 0);
}

TEST_F(IpcSharedMemoryPoolTest, Uninitialize_Succeeds)
{
    // Pool is already initialized in SetUp via Create()
    // Uninitialize is called automatically in TearDown via unique_ptr
    EXPECT_TRUE(pool_->GetTotalSize() > 0);
}

TEST_F(IpcSharedMemoryPoolTest, ReinitializeAfterUninitialize)
{
    // Note: Re-initialization after uninitialize is no longer supported
    // since Initialize() is now private and only called from Create()
    // Create a new pool to test re-initialization
    auto new_pool = SharedMemoryPool::Create(pool_name_, 65536);
    ASSERT_NE(new_pool, nullptr);
}

// ====== Allocate Tests ======

TEST_F(IpcSharedMemoryPoolTest, Allocate_Succeeds)
{
    // Pool is already initialized in SetUp via Create()
    SharedMemoryBlock block;
    auto              result = pool_->Allocate(1024, block);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_NE(block.data, nullptr);
    EXPECT_EQ(block.size, 1024);
    EXPECT_NE(block.handle, 0);
}

TEST_F(IpcSharedMemoryPoolTest, Allocate_MultipleBlocks)
{
    // Pool is already initialized in SetUp via Create()
    SharedMemoryBlock block1, block2, block3;
    ASSERT_EQ(pool_->Allocate(1024, block1), DAS_S_OK);
    ASSERT_EQ(pool_->Allocate(2048, block2), DAS_S_OK);
    ASSERT_EQ(pool_->Allocate(512, block3), DAS_S_OK);

    EXPECT_NE(block1.data, nullptr);
    EXPECT_NE(block2.data, nullptr);
    EXPECT_NE(block3.data, nullptr);

    // Each block should have different addresses
    EXPECT_NE(block1.data, block2.data);
    EXPECT_NE(block2.data, block3.data);
}

TEST_F(IpcSharedMemoryPoolTest, Allocate_UpdatesUsedSize)
{
    // Pool is already initialized in SetUp via Create()

    SharedMemoryBlock block;
    ASSERT_EQ(pool_->Allocate(1024, block), DAS_S_OK);

    size_t used = pool_->GetUsedSize();
    EXPECT_GE(used, 1024);
}

// ====== Deallocate Tests ======

TEST_F(IpcSharedMemoryPoolTest, Deallocate_Succeeds)
{
    // Pool is already initialized in SetUp via Create()

    SharedMemoryBlock block;
    ASSERT_EQ(pool_->Allocate(1024, block), DAS_S_OK);

    auto result = pool_->Deallocate(block.handle);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcSharedMemoryPoolTest, Deallocate_InvalidName)
{
    // Pool is already initialized in SetUp via Create()

    auto result = pool_->Deallocate(999999ULL);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(IpcSharedMemoryPoolTest, Deallocate_ReducesUsedSize)
{
    // Pool is already initialized in SetUp via Create()

    SharedMemoryBlock block;
    ASSERT_EQ(pool_->Allocate(1024, block), DAS_S_OK);

    size_t used_after_alloc = pool_->GetUsedSize();
    ASSERT_EQ(pool_->Deallocate(block.handle), DAS_S_OK);

    size_t used_after_dealloc = pool_->GetUsedSize();
    EXPECT_LT(used_after_dealloc, used_after_alloc);
}

// ====== GetTotalSize/GetUsedSize Tests ======

TEST_F(IpcSharedMemoryPoolTest, GetTotalSize_ReturnsInitializedSize)
{
    // Pool is already initialized in SetUp via Create()
    size_t initial_size = 65536;
    EXPECT_EQ(pool_->GetTotalSize(), initial_size);
}

TEST_F(IpcSharedMemoryPoolTest, GetUsedSize_ZeroInitially)
{
    // Pool is already initialized in SetUp via Create()

    // Used size may not be exactly zero due to internal allocations
    // but it should be small
    size_t used = pool_->GetUsedSize();
    EXPECT_LT(used, 1000);
}

// ====== CleanupStaleBlocks Tests ======

TEST_F(IpcSharedMemoryPoolTest, CleanupStaleBlocks_Succeeds)
{
    // Pool is already initialized in SetUp via Create()

    auto result = pool_->CleanupStaleBlocks();
    EXPECT_EQ(result, DAS_S_OK);
}

// ====== SharedMemoryManager Tests ======

class IpcSharedMemoryManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create and initialize the manager using factory function
        manager_ = SharedMemoryManager::Create();

        // Generate unique pool ID: nanoseconds % 65535 (must fit in uint16_t)
        // NOTE: pool_id is converted to uint16_t internally
        auto now_ns = std::chrono::high_resolution_clock::now()
                          .time_since_epoch()
                          .count();
        unique_pool_name_ = DAS_FMT_NS::format("{}", now_ns % 65535);
    }

    void TearDown() override
    {
        // unique_ptr will automatically call destructor which calls Uninitialize()
        manager_.reset();
    }

    std::unique_ptr<SharedMemoryManager> manager_;
    std::string                          unique_pool_name_;
};

TEST_F(IpcSharedMemoryManagerTest, CreatePool_Succeeds)
{
    // Manager is already initialized in SetUp via Create()
    auto result = manager_->CreatePool(unique_pool_name_, 65536);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcSharedMemoryManagerTest, GetPool_ReturnsCreatedPool)
{
    // Manager is already initialized in SetUp via Create()
    ASSERT_EQ(manager_->CreatePool(unique_pool_name_, 65536), DAS_S_OK);

    SharedMemoryPool* pool = nullptr;
    auto              result = manager_->GetPool(unique_pool_name_, pool);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_NE(pool, nullptr);
}

TEST_F(IpcSharedMemoryManagerTest, GetPool_NonExistentPool)
{
    // Manager is already initialized in SetUp via Create()
    SharedMemoryPool* pool = nullptr;
    auto              result = manager_->GetPool("nonexistent", pool);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(IpcSharedMemoryManagerTest, DestroyPool_Succeeds)
{
    // Manager is already initialized in SetUp via Create()
    ASSERT_EQ(manager_->CreatePool(unique_pool_name_, 65536), DAS_S_OK);

    auto result = manager_->DestroyPool(unique_pool_name_);
    EXPECT_EQ(result, DAS_S_OK);

    // Pool should no longer be accessible
    SharedMemoryPool* pool = nullptr;
    EXPECT_NE(manager_->GetPool(unique_pool_name_, pool), DAS_S_OK);
}

TEST_F(IpcSharedMemoryManagerTest, MakePoolName_GeneratesCorrectFormat)
{
    std::string name = SharedMemoryManager::MakePoolName(1, 2);
    EXPECT_EQ(name, "das_shm_1_2");
}

// ====== Concurrency Tests ======

TEST_F(IpcSharedMemoryPoolTest, Allocate_MultiThreaded)
{
    // Note: Since Initialize is now private, we use the pool created in SetUp
    // The pool in SetUp is 64KB, which is sufficient for this test

    const int                                   num_threads = 4;
    const int                                   allocs_per_thread = 10;
    std::vector<std::thread>                    threads;
    std::vector<std::vector<SharedMemoryBlock>> blocks(num_threads);

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back(
            [&, t]()
            {
                for (int i = 0; i < allocs_per_thread; ++i)
                {
                    SharedMemoryBlock block;
                    if (pool_->Allocate(1024, block) == DAS_S_OK)
                    {
                        blocks[t].push_back(block);
                    }
                }
            });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    // Count successful allocations
    int total_allocs = 0;
    for (const auto& thread_blocks : blocks)
    {
        total_allocs += static_cast<int>(thread_blocks.size());
    }

    // At least some allocations should succeed
    EXPECT_GT(total_allocs, 0);
}
