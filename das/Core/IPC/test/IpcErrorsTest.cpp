#include <das/Core/IPC/IpcErrors.h>
#include <das/IDasBase.h>
#include <gtest/gtest.h>

// Test IPC error code base value
TEST(IpcErrorsTest, ErrorCodeBaseValue)
{
    EXPECT_EQ(DAS_E_IPC_BASE, -1080000000);
}

// Test IPC error codes do not conflict with existing error codes
TEST(IpcErrorsTest, NoConflictWithExistingErrors)
{
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_RESERVED);
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_NO_INTERFACE);
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_UNDEFINED_RETURN_VALUE);
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_INVALID_STRING);
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_OUT_OF_MEMORY);
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_TIMEOUT);

    EXPECT_NE(DAS_E_IPC_INVALID_MESSAGE_HEADER, DAS_E_TIMEOUT);
    EXPECT_NE(DAS_E_IPC_TIMEOUT, DAS_E_TIMEOUT);

    EXPECT_LT(DAS_E_IPC_BASE, 0);
    EXPECT_LT(DAS_E_IPC_BASE, -10000000);
}

// Test error codes follow sequential pattern
TEST(IpcErrorsTest, SequentialPattern)
{
    for (int i = 0; i < 16; ++i)
    {
        int expected = DAS_E_IPC_BASE - (i + 1);
        EXPECT_LE(expected, -1073741868);
        EXPECT_GE(expected, -1073741883);
    }
}

// Test IPC error codes do not conflict with existing error codes
TEST(IpcErrorsTest, NoConflictWithExistingErrors)
{
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_RESERVED);
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_NO_INTERFACE);
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_UNDEFINED_RETURN_VALUE);
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_INVALID_STRING);
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_OUT_OF_MEMORY);
    EXPECT_NE(DAS_E_IPC_BASE, DAS_E_TIMEOUT);

    EXPECT_NE(DAS_E_IPC_INVALID_MESSAGE_HEADER, DAS_E_TIMEOUT);
    EXPECT_NE(DAS_E_IPC_TIMEOUT, DAS_E_TIMEOUT);

    EXPECT_LT(DAS_E_IPC_BASE, 0);
    EXPECT_GT(DAS_E_IPC_BASE, -10000);
}

// Test error codes follow sequential pattern
TEST(IpcErrorsTest, SequentialPattern)
{
    for (int i = 0; i < 16; ++i)
    {
        int expected = DAS_E_IPC_BASE - (i + 1);
        EXPECT_LE(expected, -2001);
        EXPECT_GE(expected, -2016);
    }
}
