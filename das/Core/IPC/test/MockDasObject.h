#ifndef DAS_CORE_IPC_TEST_MOCK_DAS_OBJECT_H
#define DAS_CORE_IPC_TEST_MOCK_DAS_OBJECT_H

#include <atomic>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>

/// 简单的 IDasBase mock 对象，用于 DistributedObjectManager 单元测试
class MockDasObject : public IDasBase
{
public:
    uint32_t DAS_STD_CALL AddRef() override
    {
        return ++ref_count_;
    }

    uint32_t DAS_STD_CALL Release() override
    {
        auto c = --ref_count_;
        if (c == 0)
        {
            delete this;
        }
        return c;
    }

    DasResult DAS_STD_CALL QueryInterface(const DasGuid&, void**) override
    {
        return DAS_E_NO_INTERFACE;
    }

    [[nodiscard]] uint32_t RefCount() const { return ref_count_.load(); }

private:
    std::atomic<uint32_t> ref_count_{0};
    virtual ~MockDasObject() = default;
};

#endif // DAS_CORE_IPC_TEST_MOCK_DAS_OBJECT_H
