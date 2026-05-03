/**
 * @file lua_director_lifecycle_test.cpp
 * @brief E2E GTest for Lua Director lifecycle validation
 *
 * Patterned after Java BridgeLifecycleDirector test.
 * Exercises complete Director lifecycle:
 *   1. Creation with correct COM ref count
 *   2. Method dispatch (C++ -> Lua override)
 *   3. Method fallback detection (no Lua override)
 *   4. COM ref counting (AddRef/Release semantics)
 *   5. QueryInterface for correct IIDs
 *   6. Lua GC -> Release -> delete chain
 *   7. Graceful error handling for Lua-side errors
 *
 * Prerequisites: DAS_BUILD_TEST=ON, EXPORT_LUA=ON
 */

#include <gtest/gtest.h>
#include <sol/sol.hpp>

#include <atomic>

// Include generated Lua export code directly — defines LuaDirector base,
// ILuaDasLogReader, ILuaDasLogRequester, ILuaDasSourceLocation,
// and registration functions (register_all_das_interfaces, etc.)
// Build system provides include path to this file.
#include "DasCore_lua_export.cpp"

// ============================================================================
// Test fixture
// ============================================================================

class LuaDirectorLifecycleTest : public ::testing::Test
{
protected:
    sol::state lua;

    void SetUp() override
    {
        lua.open_libraries(
            sol::lib::base,
            sol::lib::package,
            sol::lib::table,
            sol::lib::string);

        // Register DAS interface Director classes (ILuaDasLogReader, etc.)
        register_all_das_interfaces(lua);

        // Register DasResult enum values (DAS_S_OK, DAS_E_FAIL, etc.)
        register_error_codes(lua);

        // NOTE: register_module_functions() intentionally omitted —
        // lifecycle tests don't need DasCore runtime functions like
        // DasLogError, QueryMainProcessInterface, etc.
    }

    void TearDown() override
    {
        // Force GC to collect any Directors still alive in Lua state
        lua.collect_garbage();
    }
};

// ============================================================================
// Test 1: Director created with correct initial ref count
// ============================================================================

TEST_F(LuaDirectorLifecycleTest, DirectorCreatedWithCorrectRefCount)
{
    lua.safe_script("reader = ILuaDasLogReader({})");

    auto* ptr = lua["reader"].get<ILuaDasLogReader*>();
    ASSERT_NE(ptr, nullptr);

    // Constructor initializes ref_count_ to 1
    EXPECT_EQ(ptr->ref_count_.load(std::memory_order_acquire), 1u);

    lua["reader"] = sol::nil;
}

// ============================================================================
// Test 2: Lua method override dispatches correctly from C++ side
// ============================================================================

TEST_F(LuaDirectorLifecycleTest, LuaMethodOverrideDispatchesCorrectly)
{
    lua.safe_script(R"lua(
        override_called = false
        reader = ILuaDasLogReader({
            ReadOne = function(self, msg)
                override_called = true
                return 0  -- DAS_S_OK
            end
        })
    )lua");

    auto* ptr = lua["reader"].get<ILuaDasLogReader*>();
    ASSERT_NE(ptr, nullptr);

    // Director should detect the Lua override exists
    EXPECT_TRUE(ptr->has_lua_method("ReadOne"));

    // Call ReadOne from C++ side — should dispatch to the Lua override
    DasResult hr = ptr->ReadOne(nullptr);
    EXPECT_EQ(hr, DAS_S_OK);

    // Verify the Lua function was actually executed
    EXPECT_TRUE(lua["override_called"].get<bool>());

    lua["reader"] = sol::nil;
}

// ============================================================================
// Test 3: Director without override detects no Lua method
// ============================================================================

TEST_F(LuaDirectorLifecycleTest, DirectorWithoutOverrideDetectsNoLuaMethod)
{
    lua.safe_script("reader = ILuaDasLogReader({})");

    auto* ptr = lua["reader"].get<ILuaDasLogReader*>();
    ASSERT_NE(ptr, nullptr);

    // No Lua override was provided — Director should detect this
    EXPECT_FALSE(ptr->has_lua_method("ReadOne"));
    EXPECT_FALSE(ptr->has_lua_method("SomeNonExistentMethod"));

    // Object is still valid for non-dispatch operations
    EXPECT_EQ(ptr->ref_count_.load(std::memory_order_acquire), 1u);

    lua["reader"] = sol::nil;
}

// ============================================================================
// Test 4: Garbage collection triggers Release (GC -> Release -> delete chain)
//
// Strategy: AddRef the Director from C++ side (ref 1->2), then let Lua GC
// run. GC calls Release (ref 2->1). Object survives because we hold a ref.
// Verify ref_count == 1 after GC, then our final Release deletes it.
// ============================================================================

TEST_F(LuaDirectorLifecycleTest, GarbageCollectionTriggersRelease)
{
    lua.safe_script("reader = ILuaDasLogReader({})");

    auto* ptr = lua["reader"].get<ILuaDasLogReader*>();
    ASSERT_NE(ptr, nullptr);

    // AddRef from C++ to prevent deletion when Lua GC calls Release
    uint32_t count = ptr->AddRef();
    EXPECT_EQ(count, 2u);

    // Remove the Lua-side reference
    lua["reader"] = sol::nil;

    // Force GC — the GC metamethod calls Release()
    lua.collect_garbage();
    lua.collect_garbage(); // Second pass ensures finalizers complete

    // After GC: Lua's Release was called (ref 2->1), object still alive
    EXPECT_EQ(ptr->ref_count_.load(std::memory_order_acquire), 1u);

    // Our Release should delete the object (ref 1->0)
    count = ptr->Release();
    EXPECT_EQ(count, 0u);
    // ptr is now dangling — do not access
}

// ============================================================================
// Test 5: AddRef increments, Release decrements ref count
// ============================================================================

TEST_F(LuaDirectorLifecycleTest, AddRefIncrementsReleaseDecrementsRefCount)
{
    lua.safe_script("reader = ILuaDasLogReader({})");

    auto* ptr = lua["reader"].get<ILuaDasLogReader*>();
    ASSERT_NE(ptr, nullptr);

    // Initial: ref_count = 1
    EXPECT_EQ(ptr->ref_count_.load(std::memory_order_acquire), 1u);

    // AddRef -> ref_count = 2
    uint32_t count = ptr->AddRef();
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(ptr->ref_count_.load(std::memory_order_acquire), 2u);

    // AddRef -> ref_count = 3
    count = ptr->AddRef();
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(ptr->ref_count_.load(std::memory_order_acquire), 3u);

    // Release -> ref_count = 2
    count = ptr->Release();
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(ptr->ref_count_.load(std::memory_order_acquire), 2u);

    // Release -> ref_count = 1 (object still alive, Lua holds a ref)
    count = ptr->Release();
    EXPECT_EQ(count, 1u);

    // Let TearDown GC handle the final Release
    lua["reader"] = sol::nil;
}

// ============================================================================
// Test 6: QueryInterface returns correct interfaces
// ============================================================================

TEST_F(LuaDirectorLifecycleTest, QueryInterfaceReturnsCorrectInterface)
{
    lua.safe_script("reader = ILuaDasLogReader({})");

    auto* ptr = lua["reader"].get<ILuaDasLogReader*>();
    ASSERT_NE(ptr, nullptr);

    void* pObj = nullptr;

    // QI for IDasBase IID -> should succeed
    DasResult hr = ptr->QueryInterface(IDasBase::GetIID(), &pObj);
    EXPECT_EQ(hr, DAS_S_OK);
    ASSERT_NE(pObj, nullptr);
    static_cast<IDasBase*>(pObj)->Release();

    // QI for IDasLogReader IID -> should succeed
    pObj = nullptr;
    hr = ptr->QueryInterface(IDasLogReader::GetIID(), &pObj);
    EXPECT_EQ(hr, DAS_S_OK);
    ASSERT_NE(pObj, nullptr);
    static_cast<IDasLogReader*>(pObj)->Release();

    // QI for unknown IID -> should return E_NO_INTERFACE
    pObj = nullptr;
    DasGuid unknown_iid = {
        0xDEADBEEF,
        0x1234,
        0x5678,
        {0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44}};
    hr = ptr->QueryInterface(unknown_iid, &pObj);
    EXPECT_EQ(hr, DAS_E_NO_INTERFACE);
    EXPECT_EQ(pObj, nullptr);

    // QI with null pp_object -> should return E_INVALID_POINTER
    hr = ptr->QueryInterface(IDasBase::GetIID(), nullptr);
    EXPECT_EQ(hr, DAS_E_INVALID_POINTER);

    // Ref count unchanged after balanced QI round-trips
    // (each successful QI AddRef'd + we Released'd = net zero)
    EXPECT_EQ(ptr->ref_count_.load(std::memory_order_acquire), 1u);

    lua["reader"] = sol::nil;
}

// ============================================================================
// Test 7: Lua error in override handled gracefully
//
// When a Lua override throws, sol::protected_function_result is invalid.
// The Director catches this and returns DAS_E_FAIL instead of crashing.
// ============================================================================

TEST_F(LuaDirectorLifecycleTest, LuaErrorHandledGracefully)
{
    lua.safe_script(R"lua(
        reader = ILuaDasLogReader({
            ReadOne = function(self, msg)
                error("intentional test error")
            end
        })
    )lua");

    auto* ptr = lua["reader"].get<ILuaDasLogReader*>();
    ASSERT_NE(ptr, nullptr);

    // Call ReadOne — the Lua function throws, sol catches it
    DasResult hr = ptr->ReadOne(nullptr);

    // Director should catch the error and return DAS_E_FAIL
    EXPECT_EQ(hr, DAS_E_FAIL);

    // Director should still be alive and usable
    EXPECT_EQ(ptr->ref_count_.load(std::memory_order_acquire), 1u);

    lua["reader"] = sol::nil;
}
