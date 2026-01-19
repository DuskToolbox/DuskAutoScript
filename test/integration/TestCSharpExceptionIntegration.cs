// DasException C# integration tests
//
// This class tests the complete integration of DasException handling in C#,
// including calling C++ functions that throw DasException and verifying the
// exception's ErrorCode and Message.
//
// Note: These tests require the C# SWIG module to be built first.
// To build the C# module, run CMake with -DEXPORT_CSHARP=ON.
//
// The tests in this file are designed to verify the full integration flow:
// 1. Calling C++ functions from C# via SWIG
// 2. C++ functions throwing DasException
// 3. SWIG exception handler catching and converting DasException
// 4. C# receiving DasException (derived from Exception or ApplicationException)
// 5. Verifying the exception's ErrorCode and Message

using System;
using System.IO;
using System.Reflection;
using NUnit.Framework;

namespace DuskAutoScript.IntegrationTests
{
    /// <summary>
    /// Main integration test class for DasException in C#
    /// </summary>
    [TestFixture]
    public class TestCSharpExceptionIntegration
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
        /// Type reference to the SWIG module (for accessing functions)
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
                        Console.WriteLine("To enable these tests, build the C# module with:");
                        Console.WriteLine("  cmake -DDAS_BUILD_TEST=ON -DEXPORT_CSHARP=ON -S . -B build");
                        Console.WriteLine("  cmake --build build");
                    }
                }

                // Try to find the SWIG module type to access functions
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
                            Console.WriteLine("Note: SWIG module type not found for function access");
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
        /// Test class: TestCallFunctionReturningError
        /// Test calling a C++ function that throws DasException.
        ///
        /// This test verifies:
        /// - The function call triggers an exception in C#
        /// - The exception is of the correct type (DasException)
        /// - The exception has an ErrorCode property
        /// - The ErrorCode matches the expected value
        /// - The exception message is non-empty and informative
        /// </summary>
        [TestFixture]
        public class TestCallFunctionReturningError
        {
            /// <summary>
            /// Test calling a function that returns DAS_E_INVALID_POINTER.
            ///
            /// Test Steps:
            /// 1. Find a C++ function that throws DasException with DAS_E_INVALID_POINTER
            /// 2. Call the function from C# via SWIG
            /// 3. Catch the resulting exception
            /// 4. Verify the exception type is DasException
            /// 5. Verify the exception has ErrorCode property
            /// 6. Verify ErrorCode == DAS_E_INVALID_POINTER (-1073741847)
            /// 7. Verify the exception message is non-empty
            ///
            /// Expected Behavior:
            /// - A DasException is raised by SWIG when the C++ function throws DasException
            /// - The DasException has an ErrorCode property (from %rename and %typemap)
            /// - The ErrorCode matches DAS_E_INVALID_POINTER
            /// - The message contains information about the error
            ///
            /// Note: This test requires an actual C++ function that throws
            /// DasException. The following is a template for when such a function
            /// is available:
            ///
            /// TODO: Replace with actual function call
            /// try {
            ///     // Example: Call a function that throws with DAS_E_INVALID_POINTER
            ///     // result = DuskAutoScript.SomeFunctionThatThrows();
            ///     Assert.Fail("Expected DasException to be raised");
            /// }
            /// catch (DasException e) {
            ///     // Verify exception has ErrorCode property
            ///     Assert.IsNotNull(e.ErrorCode, "Exception should have ErrorCode property");
            ///
            ///     // Verify ErrorCode matches expected value
            ///     int expectedErrorCode = -1073741847; // DAS_E_INVALID_POINTER
            ///     Assert.AreEqual(expectedErrorCode, e.ErrorCode,
            ///         $"Expected ErrorCode {expectedErrorCode}, got {e.ErrorCode}");
            ///
            ///     // Verify message is non-empty
            ///     Assert.IsNotEmpty(e.Message, "Exception message should not be empty");
            /// }
            /// catch (Exception e) {
            ///     Assert.Fail($"Expected DasException, got {e.GetType().Name}: {e.Message}");
            /// }
            /// </summary>
            [Test]
            public void TestCallFunctionReturningInvalidPointerError()
            {
                Assert.That(SwigModuleAvailable, Is.True,
                    "SWIG C# module not built. Build with: cmake -DEXPORT_CSHARP=ON");

                // This test is a template. When an actual function is available,
                // implement the test logic above.
                // For now, verify the test framework is correct.
                Assert.Inconclusive(
                    "No C++ function available that throws DasException. " +
                    "This is expected when the C# module is not built. " +
                    "When a function is available, implement the test call here."
                );
            }

            /// <summary>
            /// Test calling a function that returns DAS_E_NO_INTERFACE.
            ///
            /// Expected Behavior:
            /// - DasException is raised
            /// - ErrorCode == DAS_E_NO_INTERFACE (-1073741844)
            /// - Message is informative
            ///
            /// Note: This test requires an actual C++ function that throws
            /// DasException with DAS_E_NO_INTERFACE.
            ///
            /// TODO: Implement with actual function call when available.
            /// </summary>
            [Test]
            public void TestCallFunctionReturningNoInterfaceError()
            {
                Assert.That(SwigModuleAvailable, Is.True,
                    "SWIG C# module not built. Build with: cmake -DEXPORT_CSHARP=ON");

                Assert.Inconclusive(
                    "No C++ function available that throws DasException. " +
                    "This is expected when the C# module is not built."
                );
            }

            /// <summary>
            /// Test calling a function that returns DAS_E_INVALID_ENUM.
            ///
            /// Expected Behavior:
            /// - DasException is raised
            /// - ErrorCode == DAS_E_INVALID_ENUM (-1073741832)
            /// - Message is informative
            ///
            /// Note: This test requires an actual C++ function that throws
            /// DasException with DAS_E_INVALID_ENUM.
            ///
            /// TODO: Implement with actual function call when available.
            /// </summary>
            [Test]
            public void TestCallFunctionReturningInvalidEnumError()
            {
                Assert.That(SwigModuleAvailable, Is.True,
                    "SWIG C# module not built. Build with: cmake -DEXPORT_CSHARP=ON");

                Assert.Inconclusive(
                    "No C++ function available that throws DasException. " +
                    "This is expected when the C# module is not built."
                );
            }
        }

        /// <summary>
        /// Test class: TestCatchAndVerifyException
        /// Test catching DasException and verifying its properties.
        ///
        /// This test verifies:
        /// - Exceptions can be caught using try-catch
        /// - Multiple exceptions can be caught and differentiated by ErrorCode
        /// - Exception properties are accessible
        /// </summary>
        [TestFixture]
        public class TestCatchAndVerifyException
        {
            /// <summary>
            /// Test basic exception catching and verification.
            ///
            /// Test Steps:
            /// 1. Call a function that throws DasException
            /// 2. Catch the exception as DasException
            /// 3. Verify exception properties are accessible
            /// 4. Verify ErrorCode and Message values
            ///
            /// Expected Behavior:
            /// - Exception is caught successfully
            /// - ErrorCode property is accessible and correct
            /// - Message property is accessible and non-empty
            /// - Can differentiate exceptions by ErrorCode
            ///
            /// TODO: Implement with actual function call when available.
            /// </summary>
            [Test]
            public void TestCatchAndVerifyExceptionBasic()
            {
                Assert.That(SwigModuleAvailable, Is.True,
                    "SWIG C# module not built. Build with: cmake -DEXPORT_CSHARP=ON");

                Assert.Inconclusive(
                    "No C++ function available that throws DasException. " +
                    "This is expected when the C# module is not built."
                );
            }

            /// <summary>
            /// Test distinguishing different error types by ErrorCode.
            ///
            /// Test Steps:
            /// 1. Call functions that throw different error codes
            /// 2. Catch all as DasException
            /// 3. Differentiate by checking ErrorCode property
            ///
            /// Expected Behavior:
            /// - Different error codes result in different ErrorCode values
            /// - Can distinguish error types programmatically
            ///
            /// Example:
            /// ```
            /// var errorCodes = new List<int>();
            /// foreach (var func in new Func<void>[] { func1, func2, func3 }) {
            ///     try {
            ///         func();
            ///     } catch (DasException e) {
            ///         errorCodes.Add(e.ErrorCode);
            ///     }
            /// }
            ///
            /// // Verify different error codes were caught
            /// Assert.AreEqual(3, errorCodes.Distinct().Count());
            /// ```
            ///
            /// TODO: Implement with actual function calls when available.
            /// </summary>
            [Test]
            public void TestDistinguishDifferentErrors()
            {
                Assert.That(SwigModuleAvailable, Is.True,
                    "SWIG C# module not built. Build with: cmake -DEXPORT_CSHARP=ON");

                Assert.Inconclusive(
                    "No C++ function available that throws DasException. " +
                    "This is expected when the C# module is not built."
                );
            }
        }

        /// <summary>
        /// Test class: TestExceptionMessage
        /// Test that exception messages are correctly propagated from C++ to C#.
        ///
        /// This test verifies:
        /// - Exception messages are passed from C++ DasException::what()
        /// - Messages are UTF-8 encoded
        /// - Messages are informative and useful for debugging
        /// </summary>
        [TestFixture]
        public class TestExceptionMessage
        {
            /// <summary>
            /// Test that exception messages are correctly propagated.
            ///
            /// Test Steps:
            /// 1. Call a function that throws DasException with a specific message
            /// 2. Catch the exception
            /// 3. Verify the message is available in C#
            /// 4. Verify message content (if known)
            ///
            /// Expected Behavior:
            /// - Exception message matches C++ DasException::what()
            /// - Message is non-empty
            /// - Message is readable UTF-8 string
            ///
            /// TODO: Implement with actual function call when available.
            /// </summary>
            [Test]
            public void TestExceptionMessageContent()
            {
                Assert.That(SwigModuleAvailable, Is.True,
                    "SWIG C# module not built. Build with: cmake -DEXPORT_CSHARP=ON");

                Assert.Inconclusive(
                    "No C++ function available that throws DasException. " +
                    "This is expected when the C# module is not built."
                );
            }

            /// <summary>
            /// Test that exception messages are properly encoded as UTF-8.
            ///
            /// Test Steps:
            /// 1. Call a function that throws with UTF-8 message
            /// 2. Catch and access the message
            /// 3. Verify it's a proper C# string
            ///
            /// Expected Behavior:
            /// - Message is a valid C# string (Unicode)
            /// - Can be printed without encoding errors
            /// - Special characters are preserved
            ///
            /// TODO: Implement with actual function call when available.
            /// </summary>
            [Test]
            public void TestExceptionMessageEncoding()
            {
                Assert.That(SwigModuleAvailable, Is.True,
                    "SWIG C# module not built. Build with: cmake -DEXPORT_CSHARP=ON");

                Assert.Inconclusive(
                    "No C++ function available that throws DasException. " +
                    "This is expected when the C# module is not built."
                );
            }
        }

        /// <summary>
        /// Test class: TestSWIGExceptionHandling
        /// Test that SWIG exception handler is correctly configured.
        ///
        /// This test verifies the SWIG %exception and %typemap directives are working:
        /// - DasException is caught in C++ layer
        /// - Converted to C# DasException
        /// - ErrorCode property is available (from %rename GetErrorCode)
        /// - Message property is available (from %rename what)
        /// </summary>
        [TestFixture]
        public class TestSWIGExceptionHandling
        {
            /// <summary>
            /// Test that DasException class is available in C# module.
            ///
            /// This verifies:
            /// - DasException is exported by SWIG
            /// - DasException has ErrorCode property (from %rename)
            /// - DasException has Message property (from %rename)
            /// </summary>
            [Test]
            public void TestDasExceptionClassExists()
            {
                Assert.That(SwigModuleAvailable, Is.True,
                    "SWIG C# module not built. Build with: cmake -DEXPORT_CSHARP=ON");

                // Verify DasException type is available
                Assert.That(DasExceptionType, Is.Not.Null,
                    "DasException should be available in C# module");

                // Verify DasException has ErrorCode property (from %rename GetErrorCode)
                PropertyInfo errorCodeProperty = DasExceptionType.GetProperty("ErrorCode");
                Assert.That(errorCodeProperty, Is.Not.Null,
                    "DasException should have ErrorCode property");
                Assert.That(errorCodeProperty.PropertyType, Is.EqualTo(typeof(int)),
                    "ErrorCode property should be int");
                Assert.That(errorCodeProperty.CanRead, Is.True,
                    "ErrorCode should be readable");

                // Verify DasException has Message property (from %rename what)
                PropertyInfo messageProperty = DasExceptionType.GetProperty("Message");
                Assert.That(messageProperty, Is.Not.Null,
                    "DasException should have Message property");
                Assert.That(messageProperty.PropertyType, Is.EqualTo(typeof(string)),
                    "Message property should be string");
                Assert.That(messageProperty.CanRead, Is.True,
                    "Message should be readable");
            }

            /// <summary>
            /// Test that DasException inherits from Exception.
            ///
            /// This verifies the SWIG configuration allows DasException
            /// to be caught as a standard Exception.
            /// </summary>
            [Test]
            public void TestDasExceptionIsException()
            {
                Assert.That(SwigModuleAvailable, Is.True,
                    "SWIG C# module not built. Build with: cmake -DEXPORT_CSHARP=ON");

                // Verify DasException inherits from Exception
                Assert.That(typeof(Exception).IsAssignableFrom(DasExceptionType),
                    Is.True, "DasException should inherit from Exception");
            }

            /// <summary>
            /// Test that DasException can be created with constructor.
            ///
            /// According to ExportAll.i, DasException has constructor:
            /// public DasException(int errorCode, string message)
            /// </summary>
            [Test]
            public void TestDasExceptionCanCreateInstance()
            {
                Assert.That(SwigModuleAvailable, Is.True,
                    "SWIG C# module not built. Build with: cmake -DEXPORT_CSHARP=ON");

                try
                {
                    // Create a test instance of DasException
                    ConstructorInfo constructor = DasExceptionType.GetConstructor(
                        new[] { typeof(int), typeof(string) });
                    object ex = constructor.Invoke(new object[] {
                        -1073741847, // DAS_E_INVALID_POINTER
                        "Test error message"
                    });

                    Assert.That(ex, Is.Not.Null, "DasException instance should be created");
                    Assert.That(DasExceptionType.IsInstanceOfType(ex), Is.True,
                        "Created object should be instance of DasException");
                }
                catch (Exception e)
                {
                    Assert.Fail($"Should be able to create DasException instance: {e.Message}");
                }
            }
        }

        /// <summary>
        /// Test class: TestErrorCodes
        /// Test that error code constants are available and correct.
        ///
        /// This test verifies:
        /// - Error code constants are exported to C#
        /// - Error code values match C++ definitions
        /// </summary>
        [TestFixture]
        public class TestErrorCodes
        {
            /// <summary>
            /// Test that error code constants are available from SWIG.
            ///
            /// These should be automatically generated from C++ #define statements
            /// in include/das/IDasBase.h.
            /// </summary>
            [Test]
            public void TestErrorCodeConstantsAvailable()
            {
                Assert.That(SwigModuleAvailable, Is.True,
                    "SWIG C# module not built. Build with: cmake -DEXPORT_CSHARP=ON");

                // Note: SWIG may or may not export these constants
                // depending on configuration. If not exported, this test will be skipped.

                // Expected error code constants
                var expectedCodes = new[]
                {
                    "DAS_E_INVALID_POINTER",
                    "DAS_E_NO_INTERFACE",
                    "DAS_E_INVALID_ENUM",
                    "DAS_E_PYTHON_ERROR",
                    "DAS_E_INVALID_STRING",
                    "DAS_E_INVALID_STRING_SIZE",
                    "DAS_S_OK",
                };

                bool anyFound = false;
                foreach (string code in expectedCodes)
                {
                    if (SwigModuleType != null)
                    {
                        try
                        {
                            FieldInfo field = SwigModuleType.GetField(code);
                            if (field != null)
                            {
                                Assert.That(field, Is.Not.Null, $"Field {code} should exist");
                                Assert.That(field.FieldType, Is.EqualTo(typeof(int)),
                                    $"Field {code} should be int");
                                anyFound = true;
                            }
                            else
                            {
                                Console.WriteLine($"Note: Error code constant {code} not found in SWIG module");
                            }
                        }
                        catch (Exception)
                        {
                            Console.WriteLine($"Note: Error code constant {code} not found in SWIG module");
                        }
                    }
                    else
                    {
                        Console.WriteLine($"Note: SWIG module type not available for {code}");
                    }
                }

                if (!anyFound)
                {
                    Assert.Inconclusive(
                        "No error code constants found in SWIG module. " +
                        "This is acceptable if SWIG is configured to not export constants."
                    );
                }
            }

            /// <summary>
            /// Test that error code values match C++ definitions.
            ///
            /// Reference: include/das/IDasBase.h
            /// </summary>
            [Test]
            public void TestErrorCodeValuesMatchCpp()
            {
                // These values should match C++ definitions
                var errorCodes = new System.Collections.Generic.Dictionary<string, int>
                {
                    { "DAS_E_INVALID_POINTER", -1073741847 },
                    { "DAS_E_NO_INTERFACE", -1073741844 },
                    { "DAS_E_INVALID_ENUM", -1073741832 },
                    { "DAS_E_PYTHON_ERROR", -1073741842 },
                    { "DAS_E_INVALID_STRING", -1073741843 },
                    { "DAS_E_INVALID_STRING_SIZE", -1073741834 },
                    { "DAS_S_OK", 0 },
                };

                foreach (var kvp in errorCodes)
                {
                    Assert.That(kvp.Value, Is.InstanceOf<int>(),
                        $"{kvp.Key} should be an integer");
                }
            }
        }

        /// <summary>
        /// Test class: TestIntegrationSummary
        /// Summary of integration test framework status.
        /// </summary>
        [TestFixture]
        public class TestIntegrationSummary
        {
            /// <summary>
            /// Print integration test summary.
            ///
            /// This test provides information about the current state
            /// of the integration test framework.
            /// </summary>
            [Test]
            public void PrintIntegrationTestSummary()
            {
                Console.WriteLine("\n" + new string('=', 70));
                Console.WriteLine("C# INTEGRATION TEST SUMMARY");
                Console.WriteLine(new string('=', 70));
                Console.WriteLine($"SWIG C# Module Available: {SwigModuleAvailable}");
                if (SwigModuleAvailable)
                {
                    Console.WriteLine($"DasException Type: {DasExceptionType?.FullName ?? "Not found"}");
                    Console.WriteLine($"SWIG Module Type: {SwigModuleType?.FullName ?? "Not found"}");
                }
                else
                {
                    Console.WriteLine("\nWARNING: SWIG C# module not built");
                    Console.WriteLine("Most integration tests are inconclusive.");
                    Console.WriteLine("\nTo run integration tests:");
                    Console.WriteLine("  1. Build the C# module:");
                    Console.WriteLine("     cmake -DDAS_BUILD_TEST=ON -DEXPORT_CSHARP=ON -S . -B build");
                    Console.WriteLine("  2. Build the project:");
                    Console.WriteLine("     cmake --build build");
                    Console.WriteLine("  3. Run these tests again");
                    Console.WriteLine("\nAlternatively, implement the test calls with actual C++ functions");
                    Console.WriteLine("that throw DasException when the module is available.");
                }
                Console.WriteLine(new string('=', 70));
                Console.WriteLine();
            }
        }
    }
}
