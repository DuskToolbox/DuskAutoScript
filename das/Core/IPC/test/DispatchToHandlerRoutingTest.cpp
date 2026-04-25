/**
 * @file DispatchToHandlerRoutingTest.cpp
 * @brief Tests verifying CONTROL_PLANE/BusinessThread context boundary.
 *
 * Validates that:
 * - ControlPlaneContext only has IO-safe fields (run_loop, header)
 * - StubContext retains BT-owned fields for business handlers
 * - IAwaitableMessageHandler accepts ControlPlaneContext (compile-time check)
 * - IMessageHandler retains StubContext (compile-time check)
 */

#include <das/Core/IPC/IMessageHandler.h>
#include <gtest/gtest.h>

#include <type_traits>

using namespace DAS;
using namespace DAS::Core::IPC;

// ============================================================================
// Compile-time verification: ControlPlaneContext has only IO-safe fields
// ============================================================================

/**
 * @brief ControlPlaneContext::run_loop is IpcRunLoop&
 */
TEST(DispatchToHandlerRoutingTest, ControlPlaneContext_RunLoopIsIpcRunLoopRef)
{
    EXPECT_TRUE(
        (std::is_same_v<decltype(ControlPlaneContext::run_loop), IpcRunLoop&>));
}

/**
 * @brief ControlPlaneContext::header is const ValidatedIPCMessageHeader&
 */
TEST(DispatchToHandlerRoutingTest, ControlPlaneContext_HeaderIsConstRef)
{
    EXPECT_TRUE((std::is_same_v<
                 decltype(ControlPlaneContext::header),
                 const ValidatedIPCMessageHeader&>));
}

/**
 * @brief sizeof(ControlPlaneContext) == sizeof two references (no extra fields)
 *
 * ControlPlaneContext should contain exactly two reference-sized members:
 * run_loop (IpcRunLoop&) and header (const ValidatedIPCMessageHeader&).
 * Any BT-owned fields would increase the size beyond this.
 */
TEST(DispatchToHandlerRoutingTest, ControlPlaneContext_SizeIsTwoReferences)
{
    // On 64-bit: two pointers = 16 bytes (references are pointer-sized)
    // On 32-bit: two pointers = 8 bytes
    constexpr size_t expected = sizeof(void*) * 2;
    EXPECT_EQ(sizeof(ControlPlaneContext), expected)
        << "ControlPlaneContext should contain exactly run_loop + header "
        << "(two reference-sized fields). Extra fields may indicate "
        << "BT-owned components leaking into the IO-safe context.";
}

// ============================================================================
// Compile-time verification: StubContext retains BT-owned fields
// ============================================================================

/**
 * @brief StubContext::object_manager is DistributedObjectManager&
 */
TEST(DispatchToHandlerRoutingTest, StubContext_HasObjectManager)
{
    EXPECT_TRUE((std::is_same_v<
                 decltype(StubContext::object_manager),
                 DistributedObjectManager&>));
}

/**
 * @brief StubContext::registry is RemoteObjectRegistry&
 */
TEST(DispatchToHandlerRoutingTest, StubContext_HasRegistry)
{
    EXPECT_TRUE((
        std::
            is_same_v<decltype(StubContext::registry), RemoteObjectRegistry&>));
}

/**
 * @brief StubContext::proxy_factory is ProxyFactory&
 */
TEST(DispatchToHandlerRoutingTest, StubContext_HasProxyFactory)
{
    EXPECT_TRUE(
        (std::is_same_v<decltype(StubContext::proxy_factory), ProxyFactory&>));
}

/**
 * @brief StubContext::business_thread is std::weak_ptr<BusinessThread>
 */
TEST(DispatchToHandlerRoutingTest, StubContext_HasBusinessThread)
{
    EXPECT_TRUE((std::is_same_v<
                 decltype(StubContext::business_thread),
                 std::weak_ptr<BusinessThread>>));
}

/**
 * @brief StubContext is significantly larger than ControlPlaneContext
 *
 * StubContext contains DOM, Registry, ProxyFactory, BusinessThread weak_ptr,
 * run_loop, header, and response_flags. It must be larger than the IO-safe
 * ControlPlaneContext which only has run_loop and header.
 */
TEST(DispatchToHandlerRoutingTest, StubContext_LargerThanControlPlaneContext)
{
    EXPECT_GT(sizeof(StubContext), sizeof(ControlPlaneContext))
        << "StubContext must be larger than ControlPlaneContext because it "
        << "carries BT-owned components (DOM, Registry, ProxyFactory, etc.)";
}

// ============================================================================
// IAwaitableMessageHandler signature verification (compile-time)
// ============================================================================

/**
 * @brief Mock IAwaitableMessageHandler using ControlPlaneContext
 *
 * If this compiles, the IAwaitableMessageHandler::HandleMessage signature
 * has been updated from StubContext& to ControlPlaneContext&.
 */
namespace
{
    struct MockAwaitableHandler : public IAwaitableMessageHandler
    {
        uint32_t ref_count_ = 0;

        [[nodiscard]]
        uint32_t AddRef() override
        {
            return ++ref_count_;
        }

        [[nodiscard]]
        uint32_t Release() override
        {
            if (--ref_count_ == 0)
            {
                delete this;
                return 0;
            }
            return ref_count_;
        }

        [[nodiscard]]
        uint32_t GetInterfaceId() const override
        {
            return 0;
        }

        boost::asio::awaitable<DasResult> HandleMessage(
            const ValidatedIPCMessageHeader& /*header*/,
            const std::vector<uint8_t>& /*body*/,
            IpcResponseSender& /*sender*/,
            ControlPlaneContext& ctx) override
        {
            (void)ctx;
            co_return DAS_S_OK;
        }
    };
} // anonymous namespace

/**
 * @brief MockAwaitableHandler can be instantiated (signature check passed)
 */
TEST(DispatchToHandlerRoutingTest, AwaitableHandler_AcceptsControlPlaneContext)
{
    auto* handler = new MockAwaitableHandler();
    auto  add_result = handler->AddRef();
    EXPECT_EQ(add_result, 1u);
    auto release_result = handler->Release();
    EXPECT_EQ(release_result, 0u);
}
