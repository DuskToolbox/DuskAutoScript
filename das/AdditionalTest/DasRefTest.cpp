#include <atomic>
#include <das/DasRef.hpp>
#include <das/IDasBase.h>
#include <gtest/gtest.h>

// Reuse COM-compliant QI test object from DasPtrTest.cpp pattern
class ITestInterface : public IDasBase
{
public:
    virtual int GetValue() const noexcept = 0;
};

DAS_DEFINE_CLASS_GUID_HOLDER(
    ITestInterface,
    0x12345678,
    0x9abc,
    0xdef0,
    0x11,
    0x22,
    0x33,
    0x44,
    0x55,
    0x66,
    0x77,
    0x88)

namespace
{
    class TestQIObject : public ITestInterface
    {
    private:
        std::atomic<uint32_t> ref_count_{0};
        int                   value_;

    public:
        explicit TestQIObject(int v = 42) : value_(v) {}

        uint32_t AddRef() override { return ++ref_count_; }
        uint32_t Release() override
        {
            const auto result = --ref_count_;
            if (ref_count_ == 0)
            {
                delete this;
                return 0;
            }
            return result;
        }
        uint32_t GetRefCount() const noexcept { return ref_count_; }

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
        int GetValue() const noexcept override { return value_; }
    };

    // === DasRef RED phase tests ===
    // These tests will NOT compile until DasRef.hpp is created (Task 2),
    // which is the expected RED phase behavior.

    // Test 1: Non-null construction
    TEST(DasRefTest, ConstructFromNonNullPointer)
    {
        auto* raw = new TestQIObject(42);
        ASSERT_EQ(raw->GetRefCount(), 0);

        DAS::DasRef<ITestInterface> ref(raw);
        // DasRef(T*) calls DasPtr(T*) which AddRefs -> refcount = 1
        EXPECT_EQ(raw->GetRefCount(), 1);
        EXPECT_EQ(ref.get().GetValue(), 42);
    }

    // Test 2: Null construction throws via T* constructor
    TEST(DasRefTest, ConstructFromNullThrows)
    {
        ITestInterface* null_ptr = nullptr;
        EXPECT_THROW(
            { DAS::DasRef<ITestInterface> ref(null_ptr); },
            std::invalid_argument);
    }

    // Test 3: RefCount on copy — copy adds another reference
    TEST(DasRefTest, RefCountOnCopy)
    {
        auto*                       raw = new TestQIObject(42);
        DAS::DasRef<ITestInterface> ref1(raw);
        ASSERT_EQ(raw->GetRefCount(), 1);

        {
            DAS::DasRef<ITestInterface> ref2(ref1);
            // Copy DasRef copies internal DasPtr (AddRef) -> refcount = 2
            EXPECT_EQ(raw->GetRefCount(), 2);
            EXPECT_EQ(ref2.get().GetValue(), 42);
        }
        // ref2 destroyed (Release) -> refcount = 1
        EXPECT_EQ(raw->GetRefCount(), 1);
    }

    // Test 4: RefCount on move — move transfers, no AddRef
    TEST(DasRefTest, RefCountOnMove)
    {
        auto*                       raw = new TestQIObject(42);
        DAS::DasRef<ITestInterface> ref1(raw);
        ASSERT_EQ(raw->GetRefCount(), 1);

        DAS::DasRef<ITestInterface> ref2(std::move(ref1));
        // Move DasRef transfers internal DasPtr (no AddRef) -> refcount still 1
        EXPECT_EQ(raw->GetRefCount(), 1);
        EXPECT_EQ(ref2.get().GetValue(), 42);
    }

    // Test 5: Reference access via get() and operator T&()
    TEST(DasRefTest, ReferenceAccess)
    {
        auto*                       raw = new TestQIObject(99);
        DAS::DasRef<ITestInterface> ref(raw);

        // get() returns T&
        EXPECT_EQ(ref.get().GetValue(), 99);

        // operator T&() via implicit conversion
        ITestInterface& iface = ref;
        EXPECT_EQ(iface.GetValue(), 99);

        // static_cast<ITestInterface&> uses operator T&()
        EXPECT_EQ(static_cast<ITestInterface&>(ref).GetValue(), 99);
    }

    // Test 6: Construct from non-empty DasPtr
    TEST(DasRefTest, FromDasPtr)
    {
        auto*                       raw = new TestQIObject(42);
        DAS::DasPtr<ITestInterface> ptr(raw);
        ASSERT_EQ(raw->GetRefCount(), 1);

        DAS::DasRef<ITestInterface> ref(std::move(ptr));
        // ptr is now empty (move transferred)
        EXPECT_EQ(ptr.Get(), nullptr);
        // ref owns the reference, refcount unchanged
        EXPECT_EQ(raw->GetRefCount(), 1);
        EXPECT_EQ(ref.get().GetValue(), 42);
    }

    // Test 7: Construct from empty DasPtr throws
    TEST(DasRefTest, FromEmptyDasPtrThrows)
    {
        DAS::DasPtr<ITestInterface> empty_ptr;
        EXPECT_THROW(
            { DAS::DasRef<ITestInterface> ref(std::move(empty_ptr)); },
            std::invalid_argument);
    }

    // Test 8: DasCRef from DasRef — const view
    TEST(DasRefTest, DasCRefFromDasRef)
    {
        auto*                       raw = new TestQIObject(42);
        DAS::DasRef<ITestInterface> ref(raw);

        DAS::DasCRef<ITestInterface> cref(ref);
        EXPECT_EQ(cref.get().GetValue(), 42);

        // operator const T&()
        const ITestInterface& iface = cref;
        EXPECT_EQ(iface.GetValue(), 42);
    }

    // Test 9: AsPtrInterop — as_ptr() for QI interop
    TEST(DasRefTest, AsPtrInterop)
    {
        auto*                       raw = new TestQIObject(42);
        DAS::DasRef<ITestInterface> ref(raw);
        ASSERT_EQ(raw->GetRefCount(), 1);

        // as_ptr() returns const DasPtr<T>& for QI
        const DAS::DasPtr<ITestInterface>& ptr = ref.as_ptr();
        EXPECT_EQ(ptr.Get(), raw);

        // Can use DasPtr::As for QI
        IDasBase* base = ptr.As<IDasBase>(DasIidOf<IDasBase>());
        EXPECT_NE(base, nullptr);
        // QI AddRef'd -> refcount = 2
        EXPECT_EQ(raw->GetRefCount(), 2);
        base->Release();
        EXPECT_EQ(raw->GetRefCount(), 1);
    }

    // Test 10: Copy semantics — both refs access same object
    TEST(DasRefTest, CopySemantics)
    {
        auto*                       raw = new TestQIObject(42);
        DAS::DasRef<ITestInterface> ref1(raw);

        DAS::DasRef<ITestInterface> ref2 = ref1;
        EXPECT_EQ(&ref1.get(), &ref2.get());
        EXPECT_EQ(ref2.get().GetValue(), 42);
        EXPECT_EQ(raw->GetRefCount(), 2);
    }

} // namespace
