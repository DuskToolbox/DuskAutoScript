#include <atomic>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>
#include <gtest/gtest.h>
#include <string_view>

// 测试用 COM 风格 QI 接口 — QI 成功时 AddRef
class ITestInterface : public IDasBase
{
public:
    virtual int GetValue() const noexcept = 0;
};

DAS_DEFINE_CLASS_GUID_HOLDER(ITestInterface, 0x12345678, 0x9abc, 0xdef0, 0x11,
    0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88)

// 用于测试 apply_impl_base 的接口（有 Apply 方法）
class ITestApplyInterface : public IDasBase
{
public:
    DAS_METHOD Apply(int x) = 0;
};

DAS_DEFINE_CLASS_GUID_HOLDER(ITestApplyInterface, 0xAABBCCDD, 0xEEFF, 0x0011,
    0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99)

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
        uint32_t   GetRefCount() const noexcept { return count_; }
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

    // COM 规范的 QI 实现 — QI 成功时 AddRef
    class TestQIObject : public ITestInterface
    {
    private:
        std::atomic<uint32_t> ref_count_{0};
        int                    value_;

    public:
        explicit TestQIObject(int v = 42) : value_(v) {}

        uint32_t AddRef() override
        {
            return ++ref_count_;
        }
        uint32_t Release() override
        {
            auto result = --ref_count_;
            if (ref_count_ == 0)
            {
                delete this;
                return 0;
            }
            return result;
        }
        uint32_t GetRefCount() const noexcept
        {
            return ref_count_;
        }
        DAS_METHOD QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            if (pp_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_object = static_cast<IDasBase*>(this);
            }
            else if (iid == DasIidOf<ITestInterface>())
            {
                *pp_object = static_cast<ITestInterface*>(this);
            }
            else
            {
                *pp_object = nullptr;
                return DAS_E_NO_INTERFACE;
            }
            AddRef();
            return DAS_S_OK;
        }
        int GetValue() const noexcept override
        {
            return value_;
        }
    };

    // === Task 1 TDD 测试 ===

    // Test 1: As(DasPtr<Other>&) 使用 Attach 模式接管 QI 返回指针，
    // QI 已 AddRef，Attach 不再加
    TEST(DasPtrAsTest, AsDasPtrRefUsesAttach)
    {
        auto* raw = new TestQIObject(42);
        DAS::DasPtr<ITestInterface> ptr(raw);
        // DasPtr 构造 AddRef -> ref_count = 1
        ASSERT_EQ(raw->GetRefCount(), 1);

        DAS::DasPtr<IDasBase> base_ptr;
        const DasResult result = ptr.As(base_ptr);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_NE(base_ptr.Get(), nullptr);
        // ptr(1) + QI AddRef(1) = 2, Attach 不加
        EXPECT_EQ(raw->GetRefCount(), 2);

        // base_ptr Release -> ref_count = 1
        base_ptr.Reset();
        EXPECT_EQ(raw->GetRefCount(), 1);
    }

    // Test 2: As(Other**) 不额外 AddRef（QI 已 AddRef）
    TEST(DasPtrAsTest, AsRawPtrOutNoExtraAddRef)
    {
        auto* raw = new TestQIObject(42);
        DAS::DasPtr<ITestInterface> ptr(raw);
        ASSERT_EQ(raw->GetRefCount(), 1);

        IDasBase* base_raw = nullptr;
        const DasResult result = ptr.As(&base_raw);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_NE(base_raw, nullptr);
        // ptr(1) + QI AddRef(1) = 2, As 不额外 AddRef
        EXPECT_EQ(raw->GetRefCount(), 2);

        // 调用方负责 Release
        base_raw->Release();
        EXPECT_EQ(raw->GetRefCount(), 1);
    }

    // Test 3: As(const DasGuid&) 不对原始 ptr_ AddRef，由 QI 返回对象自带引用
    TEST(DasPtrAsTest, AsGuidReturnsQIResultWithRef)
    {
        auto* raw = new TestQIObject(42);
        DAS::DasPtr<ITestInterface> ptr(raw);
        ASSERT_EQ(raw->GetRefCount(), 1);

        ITestInterface* iface =
            ptr.As<ITestInterface>(DasIidOf<ITestInterface>());
        EXPECT_NE(iface, nullptr);
        // ptr(1) + QI AddRef(1) = 2, As 不额外 AddRef
        EXPECT_EQ(raw->GetRefCount(), 2);

        // 调用方 Release
        iface->Release();
        EXPECT_EQ(raw->GetRefCount(), 1);
    }

    // Test 4: As(const DasGuid&) QI 失败返回 nullptr
    TEST(DasPtrAsTest, AsGuidQIFailureReturnsNullptr)
    {
        auto* raw = new TestQIObject(42);
        DAS::DasPtr<ITestInterface> ptr(raw);

        DasGuid unknown_guid = {0xDEAD, 0xBEEF, 0xCAFE, {}};
        auto* iface = ptr.As<ITestInterface>(unknown_guid);
        EXPECT_EQ(iface, nullptr);
        // QI 失败不 AddRef，ref_count 不变
        EXPECT_EQ(raw->GetRefCount(), 1);
    }

    // Test 5: nullptr ptr_ 返回正确错误码
    TEST(DasPtrAsTest, NullptrReturnsCorrectErrors)
    {
        DAS::DasPtr<ITestInterface> null_ptr;

        DAS::DasPtr<IDasBase> base_ptr;
        EXPECT_EQ(null_ptr.As(base_ptr), DAS_E_INVALID_POINTER);

        IDasBase* raw_out = nullptr;
        EXPECT_EQ(null_ptr.As(&raw_out), DAS_E_NO_INTERFACE);

        auto* guid_out =
            null_ptr.As<ITestInterface>(DasIidOf<ITestInterface>());
        EXPECT_EQ(guid_out, nullptr);
    }

    // Test 6: apply_impl_base<false> 堆对象 QI 成功时 AddRef，
    // DasPtr::As(DasPtr&) 使用 Attach 不双重 AddRef
    TEST(DasPtrAsTest, ApplyImplBaseHeapQIAddRefs)
    {
        using ApplyWrapper =
            DAS::Utils::Details::apply_impl_base<false, ITestApplyInterface,
                std::function<DasResult(int)>, DasResult, int>::apply_impl;

        auto* raw = new ApplyWrapper([](int x) { return DAS_S_OK; });
        // RefCounter 初始值 0, DasPtr 构造 AddRef -> ref_count = 1
        DAS::DasPtr<ITestApplyInterface> ptr(raw);

        DAS::DasPtr<IDasBase> base_ptr;
        const DasResult result = ptr.As(base_ptr);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_NE(base_ptr.Get(), nullptr);

        // base_ptr Release -> ref_count = 1, ptr 仍持有
        base_ptr.Reset();

        // ptr 析构 -> ref_count = 0, 对象被 delete
        // 如果没有崩溃或 ASAN 报错，则行为正确
    }

    // Test 7: apply_impl_base<true> 栈对象 QI 成功不崩溃
    TEST(DasPtrAsTest, ApplyImplBaseStackQIDoesNotCrash)
    {
        using ApplyWrapper =
            DAS::Utils::Details::apply_impl_base<true, ITestApplyInterface,
                std::function<DasResult(int)>, DasResult, int>::apply_impl;

        auto lambda = [](int x) { return DAS_S_OK; };
        ApplyWrapper stack_obj(std::ref(lambda));

        void* result = nullptr;
        const DasResult qi_result = stack_obj.QueryInterface(
            DasIidOf<ITestApplyInterface>(), &result);

        EXPECT_EQ(qi_result, DAS_S_OK);
        EXPECT_NE(result, nullptr);
        // OnStack=true 的 AddRef/Release 返回固定值 1，不崩溃
        EXPECT_EQ(stack_obj.AddRef(), 1);
        EXPECT_EQ(stack_obj.Release(), 1);
    }

    // Test 8: apply_impl_base<false> QI 到不同接口（IDasBase）不崩溃
    TEST(DasPtrAsTest, ApplyImplBaseHeapQIToBaseInterface)
    {
        using ApplyWrapper =
            DAS::Utils::Details::apply_impl_base<false, ITestApplyInterface,
                std::function<DasResult(int)>, DasResult, int>::apply_impl;

        auto* raw = new ApplyWrapper([](int x) { return DAS_S_OK; });
        DAS::DasPtr<ITestApplyInterface> ptr(raw);

        // QI 到 IDasBase（不同的接口）
        DAS::DasPtr<IDasBase> base_ptr;
        const DasResult result = ptr.As(base_ptr);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_NE(base_ptr.Get(), nullptr);

        // base_ptr Release，ptr 仍持有
        base_ptr.Reset();
        // ptr 析构 -> delete，无崩溃
    }

} // namespace
