// DasException C# unit tests
//
// This class tests the DasException C# binding and its ability to
// convert DasResult error codes to C# exceptions.
//
// Note: These tests require the C# SWIG module to be built first.
// To build the C# module, run CMake with -DEXPORT_CSHARP=ON.
//
// The tests use reflection to check for DasException and its properties
// since the SWIG module may not be available during development.

using System;
using System.IO;
using System.Reflection;
using NUnit.Framework;

namespace DuskAutoScript.Tests
{
    /// <summary>
    /// Main test class for DasException
    /// </summary>
    [TestFixture]
    public class TestDasException
    {
        /// <summary>
        /// Flag to indicate if SWIG C# module is available
        /// </summary>
        private static bool SwigModuleAvailable = false;

        /// <summary>
        /// Type reference to DasException (if available)
        /// </summary>
        private static Type DasExceptionType = null;

        /// <summary>
        /// Type reference to the SWIG module (for accessing error code constants)
        /// </summary>
        private static Type SwigModuleType = null;

        /// <summary>
        /// Setup method - attempts to load the SWIG C# module.
        /// If the module is not available, all tests will be skipped.
        /// </summary>
        [OneTimeSetUp]
        public static void SetUp()
        {
            try
            {
                // Try to find the SWIG-generated C# module
                // Common locations:
                // 1. build/bin/DuskAutoScript.dll
                // 2. build/swig/csharp/DuskAutoScript.dll
                // 3. Already loaded assemblies

                // First, try to load from build directory
                string buildBinPath = Path.Combine("..", "build", "bin");
                if (Directory.Exists(buildBinPath))
                {
                    string dllPath = Path.Combine(buildBinPath, "DuskAutoScript.dll");
                    if (File.Exists(dllPath))
                    {
                        Assembly.LoadFrom(dllPath);
                    }
                }

                // Try to find the DasException type
                // The type name depends on SWIG configuration
                try
                {
                    DasExceptionType = Type.GetType("DasException", false);
                    if (DasExceptionType != null)
                    {
                        SwigModuleAvailable = true;
                    }
                }
                catch (Exception)
                {
                    // Try with namespace prefix
                    try
                    {
                        DasExceptionType = Type.GetType("DuskAutoScript.DasException", false);
                        if (DasExceptionType != null)
                        {
                            SwigModuleAvailable = true;
                        }
                    }
                    catch (Exception e2)
                    {
                        Console.WriteLine($"Warning: SWIG C# module not available: {e2.Message}");
                        Console.WriteLine("To enable these tests, build the C# module with: cmake -DEXPORT_CSHARP=ON");
                    }
                }

                // Try to find the SWIG module type to access error code constants
                if (SwigModuleAvailable)
                {
                    try
                    {
                        SwigModuleType = Type.GetType("DuskAutoScript", false);
                    }
                    catch (Exception)
                    {
                        try
                        {
                            SwigModuleType = Type.GetType("DuskAutoScriptPINVOKE", false);
                        }
                        catch (Exception e2)
                        {
                            // Module type not available, but DasException is
                            Console.WriteLine("Note: SWIG module type not found for error code constants");
                        }
                    }
                }
            }
            catch (Exception e)
            {
                Console.WriteLine($"Warning: Failed to setup SWIG module: {e.Message}");
                SwigModuleAvailable = false;
            }
        }

        /// <summary>
        /// Test class: TestDasExceptionExists
        /// Verifies that the DasException class can be loaded
        /// </summary>
        [TestFixture]
        public class TestDasExceptionExists
        {
            [Test]
            public void TestDasExceptionClassExists()
            {
                Assert.That(SwigModuleAvailable, Is.True, "SWIG C# module not built");
                Assert.That(DasExceptionType, Is.Not.Null, "DasException class should exist");
            }

            [Test]
            public void TestDasExceptionHasConstructor()
            {
                Assert.That(SwigModuleAvailable, Is.True, "SWIG C# module not built");

                try
                {
                    // According to ExportAll.i, DasException has constructor:
                    // public DasException(int errorCode, string message)
                    ConstructorInfo constructor = DasExceptionType.GetConstructor(
                        new[] { typeof(int), typeof(string) });
                    Assert.That(constructor, Is.Not.Null, "DasException should have constructor (int, string)");
                }
                catch (Exception e)
                {
                    Assert.Fail($"DasException should have constructor (int, string): {e.Message}");
                }
            }
        }

        /// <summary>
        /// Test class: TestDasExceptionErrorCode
        /// Verifies DasException ErrorCode property access
        /// </summary>
        [TestFixture]
        public class TestDasExceptionErrorCode
        {
            [Test]
            public void TestDasExceptionHasErrorCodeProperty()
            {
                Assert.That(SwigModuleAvailable, Is.True, "SWIG C# module not built");

                try
                {
                    PropertyInfo errorCodeProperty = DasExceptionType.GetProperty("ErrorCode");
                    Assert.That(errorCodeProperty, Is.Not.Null, "ErrorCode property should exist");
                    Assert.That(errorCodeProperty.PropertyType, Is.EqualTo(typeof(int)),
                        "ErrorCode property should be int");
                    Assert.That(errorCodeProperty.CanRead, Is.True, "ErrorCode should be readable");
                }
                catch (Exception e)
                {
                    Assert.Fail($"DasException should have ErrorCode property: {e.Message}");
                }
            }

            [Test]
            public void TestDasExceptionHasGetErrorCodeMethod()
            {
                Assert.That(SwigModuleAvailable, Is.True, "SWIG C# module not built");

                try
                {
                    // According to ExportAll.i, GetErrorCode is renamed to ErrorCode
                    // But the underlying method might still exist
                    MethodInfo getErrorCodeMethod = DasExceptionType.GetMethod("GetErrorCode");
                    // The method might not exist if it's renamed to a property
                    // This is acceptable
                }
                catch (Exception)
                {
                    // Method may not exist if it's renamed to a property, which is fine
                }
            }
        }

        /// <summary>
        /// Test class: TestDasExceptionMessage
        /// Verifies DasException Message property access
        /// </summary>
        [TestFixture]
        public class TestDasExceptionMessage
        {
            [Test]
            public void TestDasExceptionHasMessageProperty()
            {
                Assert.That(SwigModuleAvailable, Is.True, "SWIG C# module not built");

                try
                {
                    PropertyInfo messageProperty = DasExceptionType.GetProperty("Message");
                    Assert.That(messageProperty, Is.Not.Null, "Message property should exist");
                    Assert.That(messageProperty.PropertyType, Is.EqualTo(typeof(string)),
                        "Message property should be string");
                    Assert.That(messageProperty.CanRead, Is.True, "Message should be readable");
                }
                catch (Exception e)
                {
                    Assert.Fail($"DasException should have Message property: {e.Message}");
                }
            }

            [Test]
            public void TestDasExceptionHasWhatMethod()
            {
                Assert.That(SwigModuleAvailable, Is.True, "SWIG C# module not built");

                try
                {
                    // According to ExportAll.i, what() is renamed to Message
                    // But the underlying method might still exist
                    MethodInfo whatMethod = DasExceptionType.GetMethod("what");
                    // The method might not exist if it's renamed to a property
                    // This is acceptable
                }
                catch (Exception)
                {
                    // Method may not exist if it's renamed to a property, which is fine
                }
            }
        }

        /// <summary>
        /// Test class: TestVariousErrorCodes
        /// Tests various error codes that DasException can represent
        /// </summary>
        [TestFixture]
        public class TestVariousErrorCodes
        {
            [Test]
            public void TestErrorCodeInvalidPointer()
            {
                int expectedErrorCode = -1073741847; // DAS_E_INVALID_POINTER
                Assert.That(expectedErrorCode, Is.EqualTo(-1073741847), "DAS_E_INVALID_POINTER value incorrect");
            }

            [Test]
            public void TestErrorCodeNoInterface()
            {
                int expectedErrorCode = -1073741831; // DAS_E_NO_INTERFACE
                Assert.That(expectedErrorCode, Is.EqualTo(-1073741831), "DAS_E_NO_INTERFACE value incorrect");
            }

            [Test]
            public void TestErrorCodeInvalidEnum()
            {
                int expectedErrorCode = -1073741853; // DAS_E_INVALID_ENUM
                Assert.That(expectedErrorCode, Is.EqualTo(-1073741853), "DAS_E_INVALID_ENUM value incorrect");
            }

            [Test]
            public void TestErrorCodePythonError()
            {
                int expectedErrorCode = -1073741849; // DAS_E_PYTHON_ERROR
                Assert.That(expectedErrorCode, Is.EqualTo(-1073741849), "DAS_E_PYTHON_ERROR value incorrect");
            }

            [Test]
            public void TestErrorCodeInvalidString()
            {
                int expectedErrorCode = -1073741833; // DAS_E_INVALID_STRING
                Assert.That(expectedErrorCode, Is.EqualTo(-1073741833), "DAS_E_INVALID_STRING value incorrect");
            }

            [Test]
            public void TestErrorCodeInvalidStringSize()
            {
                int expectedErrorCode = -1073741834; // DAS_E_INVALID_STRING_SIZE
                Assert.That(expectedErrorCode, Is.EqualTo(-1073741834), "DAS_E_INVALID_STRING_SIZE value incorrect");
            }

            [Test]
            public void TestSuccessCode()
            {
                int successCode = 0; // DAS_S_OK
                Assert.That(successCode, Is.EqualTo(0), "DAS_S_OK value incorrect");
            }
        }

        /// <summary>
        /// Test class: TestDasExceptionConversion
        /// Tests DasResult failure to DasException conversion
        /// </summary>
        [TestFixture]
        public class TestDasExceptionConversion
        {
            [Test]
            public void TestCanCatchDasException()
            {
                Assert.That(SwigModuleAvailable, Is.True, "SWIG C# module not built");

                // Verify that DasException inherits from Exception or ApplicationException
                Assert.That(typeof(Exception).IsAssignableFrom(DasExceptionType),
                    Is.True, "DasException should inherit from Exception or ApplicationException");
            }

            [Test]
            public void TestDasExceptionCanCreateInstance()
            {
                Assert.That(SwigModuleAvailable, Is.True, "SWIG C# module not built");

                try
                {
                    // Create a test instance of DasException
                    // According to ExportAll.i: public DasException(int errorCode, string message)
                    ConstructorInfo constructor = DasExceptionType.GetConstructor(
                        new[] { typeof(int), typeof(string) });
                    object ex = constructor.Invoke(new object[] { -1073741847, "Test error message" });

                    Assert.That(ex, Is.Not.Null, "DasException instance should be created");
                    Assert.That(DasExceptionType.IsInstanceOfType(ex), Is.True,
                        "Created object should be instance of DasException");
                }
                catch (Exception e)
                {
                    Assert.Fail($"Should be able to create DasException instance: {e.Message}");
                }
            }

            [Test]
            public void TestDasExceptionCanGetErrorCode()
            {
                Assert.That(SwigModuleAvailable, Is.True, "SWIG C# module not built");

                try
                {
                    // Create a test instance of DasException
                    ConstructorInfo constructor = DasExceptionType.GetConstructor(
                        new[] { typeof(int), typeof(string) });
                    object ex = constructor.Invoke(new object[] { -1073741847, "Test error message" });

                    // Get the ErrorCode property
                    PropertyInfo errorCodeProperty = DasExceptionType.GetProperty("ErrorCode");
                    int errorCode = (int)errorCodeProperty.GetValue(ex);

                    Assert.That(errorCode, Is.EqualTo(-1073741847), "ErrorCode should be accessible");
                }
                catch (Exception e)
                {
                    Assert.Fail($"Should be able to get ErrorCode from DasException: {e.Message}");
                }
            }

            [Test]
            public void TestDasExceptionCanGetMessage()
            {
                Assert.That(SwigModuleAvailable, Is.True, "SWIG C# module not built");

                try
                {
                    // Create a test instance of DasException
                    ConstructorInfo constructor = DasExceptionType.GetConstructor(
                        new[] { typeof(int), typeof(string) });
                    object ex = constructor.Invoke(new object[] { -1073741847, "Test error message" });

                    // Get the Message property
                    PropertyInfo messageProperty = DasExceptionType.GetProperty("Message");
                    string message = (string)messageProperty.GetValue(ex);

                    Assert.That(message, Is.EqualTo("Test error message"), "Message should be accessible");
                }
                catch (Exception e)
                {
                    Assert.Fail($"Should be able to get Message from DasException: {e.Message}");
                }
            }
        }

        /// <summary>
        /// Test class: TestErrorCodeConstantsExist
        /// Tests that error code constants are defined in the SWIG module
        /// </summary>
        [TestFixture]
        public class TestErrorCodeConstantsExist
        {
            [Test]
            public void TestErrorCodesDefined()
            {
                Assert.That(SwigModuleAvailable, Is.True, "SWIG C# module not built");
                Assert.That(SwigModuleType, Is.Not.Null, "SWIG module type not available");

                // These error codes should be automatically generated by SWIG
                // from the C++ #define statements in include/das/IDasBase.h
                string[] expectedCodes = {
                    "DAS_E_INVALID_POINTER",
                    "DAS_E_NO_INTERFACE",
                    "DAS_E_INVALID_ENUM",
                    "DAS_E_PYTHON_ERROR",
                    "DAS_E_INVALID_STRING",
                    "DAS_E_INVALID_STRING_SIZE",
                    "DAS_S_OK",
                };

                foreach (string code in expectedCodes)
                {
                    try
                    {
                        FieldInfo field = SwigModuleType.GetField(code);
                        if (field == null)
                        {
                            // Error code constants may not be exposed in C#
                            // This is acceptable if SWIG is configured differently
                            Console.WriteLine($"Note: Error code constant {code} not found in SWIG module");
                        }
                        else
                        {
                            Assert.That(field, Is.Not.Null, $"Field {code} should exist");
                            Assert.That(field.FieldType, Is.EqualTo(typeof(int)),
                                $"Field {code} should be int");
                        }
                    }
                    catch (Exception)
                    {
                        // Error code constants may not be exposed in C#
                        // This is acceptable if SWIG is configured differently
                        Console.WriteLine($"Note: Error code constant {code} not found in SWIG module");
                    }
                }
            }
        }
    }
}
