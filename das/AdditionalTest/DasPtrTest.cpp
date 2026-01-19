#include <atomic>
#include <string_view>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <gtest/gtest.h>

namespace
{
    class TestImpl : public IDasBase
    {
    private:
        std::atomic_uint32_t count_{0};

    public:
                     TestImpl() {}
        uint32_t AddRef() override
        {
            count_ += 1;
            return count_;
        }
        uint32_t Release() override
        {
            count_ -= 1;
            if (count_ == 0)
            {
                delete this;
                return 0;
            }
            return count_;
        }
        uint32_t GetRefCount() const noexcept { return count_; }
        DAS_METHOD QueryInterface(const DasGuid& id, void** pp_object) override
        {
            *pp_object = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    DasResult MakeTestImpl(void** out_ptr)
    {
        auto* result = new TestImpl();
        static_cast<IDasBase*>(result)->AddRef();
        *out_ptr = result;
        return DAS_S_OK;
    }

    TEST(DASPtrTest, BasicTest)
    {
        TestImpl* p_test_impl = nullptr;
        {
            DAS::DasPtr<TestImpl> impl;

            const DasResult result = MakeTestImpl(impl.PutVoid());
            EXPECT_EQ(result, DAS_S_OK);
            p_test_impl = impl.Get();
            EXPECT_EQ(impl->GetRefCount(), 1);
            impl->AddRef();
            EXPECT_EQ(impl->GetRefCount(), 2);
        }
        EXPECT_EQ(p_test_impl->GetRefCount(), 1);
        p_test_impl->Release();
    }
}