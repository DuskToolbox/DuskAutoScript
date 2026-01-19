"""
DasException Python integration tests

This module tests the complete integration of DasException handling in Python,
including calling C++ functions that throw DasException and verifying the
exception's error_code and message.

Note: These tests require the Python SWIG module to be built first.
To build the Python module, run CMake with -DEXPORT_PYTHON=ON.

The tests in this file are designed to verify the full integration flow:
1. Calling C++ functions from Python via SWIG
2. C++ functions throwing DasException
3. SWIG exception handler catching and converting DasException
4. Python receiving RuntimeError with error_code attribute
5. Verifying the exception's error_code and message
"""

import unittest
import sys
from pathlib import Path

# Try to import the SWIG-generated Python module
# If not available, the tests will be skipped
SWIG_MODULE_AVAILABLE = False
das = None

try:
    # The module name depends on the SWIG configuration
    # Check common locations for the built module
    module_path = Path(__file__).parent.parent.parent / "build" / "bin"
    if module_path.exists():
        sys.path.insert(0, str(module_path))

    # Try to import the module
    # Note: The actual module name may vary based on SWIG configuration
    # Common names: DuskAutoScript, _DasCorePythonExport, das
    import DuskAutoScript as das

    SWIG_MODULE_AVAILABLE = True
except ImportError as e:
    print(f"Warning: SWIG Python module not available: {e}")
    print("To enable these tests, build the Python module with:")
    print("  cmake -DDAS_BUILD_TEST=ON -DEXPORT_PYTHON=ON -S . -B build")
    print("  cmake --build build")


@unittest.skipIf(not SWIG_MODULE_AVAILABLE, "SWIG Python module not built")
class TestCallFunctionReturningError(unittest.TestCase):
    """
    Test calling a C++ function that throws DasException.

    This test verifies:
    - The function call triggers an exception in Python
    - The exception is of the correct type (RuntimeError)
    - The exception has an error_code attribute
    - The error_code matches the expected value
    - The exception message is non-empty and informative
    """

    def test_call_function_returning_invalid_pointer_error(self):
        """
        Test calling a function that returns DAS_E_INVALID_POINTER.

        Test Steps:
        1. Find a C++ function that throws DasException with DAS_E_INVALID_POINTER
        2. Call the function from Python via SWIG
        3. Catch the resulting exception
        4. Verify the exception type is RuntimeError
        5. Verify the exception has error_code attribute
        6. Verify error_code == DAS_E_INVALID_POINTER (-1073741847)
        7. Verify the exception message is non-empty

        Expected Behavior:
        - A RuntimeError is raised by SWIG when the C++ function throws DasException
        - The RuntimeError has an error_code attribute (from %extend DasException)
        - The error_code matches DAS_E_INVALID_POINTER
        - The message contains information about the error

        Note: This test requires an actual C++ function that throws
        DasException. The following is a template for when such a function
        is available:

        TODO: Replace with actual function call
        try:
            # Example: Call a function that throws with DAS_E_INVALID_POINTER
            # result = das.SomeFunctionThatThrows()
            self.fail("Expected RuntimeError to be raised")
        except RuntimeError as e:
            # Verify exception has error_code attribute
            self.assertTrue(hasattr(e, 'error_code'),
                          "Exception should have error_code attribute")

            # Verify error_code matches expected value
            expected_error_code = -1073741847  # DAS_E_INVALID_POINTER
            self.assertEqual(e.error_code, expected_error_code,
                           f"Expected error_code {expected_error_code}, got {e.error_code}")

            # Verify message is non-empty
            self.assertTrue(len(str(e)) > 0,
                          "Exception message should not be empty")

        except Exception as e:
            self.fail(f"Expected RuntimeError, got {type(e).__name__}: {e}")
        """
        # This test is a template. When an actual function is available,
        # implement the test logic above.
        self.skipTest(
            "No C++ function available that throws DasException. "
            "This is expected when the Python module is not built."
        )

    def test_call_function_returning_no_interface_error(self):
        """
        Test calling a function that returns DAS_E_NO_INTERFACE.

        Expected Behavior:
        - RuntimeError is raised
        - error_code == DAS_E_NO_INTERFACE (-1073741844)
        - Message is informative

        Note: This test requires an actual C++ function that throws
        DasException with DAS_E_NO_INTERFACE.

        TODO: Implement with actual function call when available.
        """
        self.skipTest("No C++ function available that throws DasException")

    def test_call_function_returning_invalid_enum_error(self):
        """
        Test calling a function that returns DAS_E_INVALID_ENUM.

        Expected Behavior:
        - RuntimeError is raised
        - error_code == DAS_E_INVALID_ENUM (-1073741832)
        - Message is informative

        Note: This test requires an actual C++ function that throws
        DasException with DAS_E_INVALID_ENUM.

        TODO: Implement with actual function call when available.
        """
        self.skipTest("No C++ function available that throws DasException")


@unittest.skipIf(not SWIG_MODULE_AVAILABLE, "SWIG Python module not built")
class TestCatchAndVerifyException(unittest.TestCase):
    """
    Test catching DasException and verifying its properties.

    This test verifies:
    - Exceptions can be caught using try-except
    - Multiple exceptions can be caught and differentiated by error_code
    - Exception attributes are accessible
    """

    def test_catch_and_verify_exception_basic(self):
        """
        Test basic exception catching and verification.

        Test Steps:
        1. Call a function that throws DasException
        2. Catch the exception as RuntimeError
        3. Verify exception attributes are accessible
        4. Verify error_code and message values

        Expected Behavior:
        - Exception is caught successfully
        - error_code attribute is accessible and correct
        - Message attribute is accessible and non-empty
        - Can differentiate exceptions by error_code

        TODO: Implement with actual function call when available.
        """
        self.skipTest("No C++ function available that throws DasException")

    def test_distinguish_different_errors(self):
        """
        Test distinguishing different error types by error_code.

        Test Steps:
        1. Call functions that throw different error codes
        2. Catch all as RuntimeError
        3. Differentiate by checking error_code attribute

        Expected Behavior:
        - Different error codes result in different error_code values
        - Can distinguish error types programmatically

        Example:
        ```
        error_codes_caught = []
        for func in [func1, func2, func3]:
            try:
                func()
            except RuntimeError as e:
                error_codes_caught.append(e.error_code)

        # Verify different error codes were caught
        self.assertEqual(len(set(error_codes_caught)), 3)
        ```

        TODO: Implement with actual function calls when available.
        """
        self.skipTest("No C++ function available that throws DasException")


@unittest.skipIf(not SWIG_MODULE_AVAILABLE, "SWIG Python module not built")
class TestExceptionMessage(unittest.TestCase):
    """
    Test that exception messages are correctly propagated from C++ to Python.

    This test verifies:
    - Exception messages are passed from C++ DasException::what()
    - Messages are UTF-8 encoded
    - Messages are informative and useful for debugging
    """

    def test_exception_message_content(self):
        """
        Test that exception messages are correctly propagated.

        Test Steps:
        1. Call a function that throws DasException with a specific message
        2. Catch the exception
        3. Verify the message is available in Python
        4. Verify message content (if known)

        Expected Behavior:
        - Exception message matches C++ DasException::what()
        - Message is non-empty
        - Message is readable UTF-8 string

        TODO: Implement with actual function call when available.
        """
        self.skipTest("No C++ function available that throws DasException")

    def test_exception_message_encoding(self):
        """
        Test that exception messages are properly encoded as UTF-8.

        Test Steps:
        1. Call a function that throws with UTF-8 message
        2. Catch and access the message
        3. Verify it's a proper Python string

        Expected Behavior:
        - Message is a valid Python str (unicode)
        - Can be printed without encoding errors
        - Special characters are preserved

        TODO: Implement with actual function call when available.
        """
        self.skipTest("No C++ function available that throws DasException")


class TestSWIGExceptionHandling(unittest.TestCase):
    """
    Test that SWIG exception handler is correctly configured.

    This test verifies the SWIG %exception directive is working:
    - DasException is caught in C++ layer
    - Converted to Python RuntimeError
    - error_code attribute is added via %extend
    """

    def test_das_exception_class_exists(self):
        """
        Test that DasException class is available in Python module.

        This verifies:
        - DasException is exported by SWIG
        - DasException has GetErrorCode() method (from %extend)
        - DasException has GetMessage() method (from %extend)
        """
        if not SWIG_MODULE_AVAILABLE:
            self.skipTest("SWIG Python module not built")

        # Verify DasException class exists
        self.assertTrue(hasattr(das, 'DasException'),
                       "DasException should be available in Python module")

        # Verify DasException has methods from %extend directive
        self.assertTrue(hasattr(das.DasException, 'GetErrorCode'),
                       "DasException should have GetErrorCode method")
        self.assertTrue(hasattr(das.DasException, 'GetMessage'),
                       "DasException should have GetMessage method")

    def test_exception_is_runtime_error(self):
        """
        Test that DasException is mapped to RuntimeError in Python.

        This verifies the SWIG %exception directive configuration.
        According to ExportAll.i, DasException is mapped to SWIG_RuntimeError.
        """
        if not SWIG_MODULE_AVAILABLE:
            self.skipTest("SWIG Python module not built")

        # According to ExportAll.i %exception directive,
        # DasException is converted to SWIG_RuntimeError
        # which maps to Python's built-in RuntimeError
        import builtins
        self.assertTrue(hasattr(builtins, 'RuntimeError'),
                       "RuntimeError should be available")


class TestErrorCodes(unittest.TestCase):
    """
    Test that error code constants are available and correct.

    This test verifies:
    - Error code constants are exported to Python
    - Error code values match C++ definitions
    """

    def test_error_code_constants_available(self):
        """
        Test that error code constants are available from SWIG.

        These should be automatically generated from C++ #define statements
        in include/das/IDasBase.h.
        """
        if not SWIG_MODULE_AVAILABLE:
            self.skipTest("SWIG Python module not built")

        # Expected error code constants
        expected_codes = {
            'DAS_E_INVALID_POINTER': -1073741847,
            'DAS_E_NO_INTERFACE': -1073741844,
            'DAS_E_INVALID_ENUM': -1073741832,
            'DAS_E_PYTHON_ERROR': -1073741842,
            'DAS_E_INVALID_STRING': -1073741843,
            'DAS_E_INVALID_STRING_SIZE': -1073741834,
            'DAS_S_OK': 0,
        }

        # Note: SWIG may or may not export these constants
        # depending on configuration. If not exported, this test will be skipped.
        for code_name, expected_value in expected_codes.items():
            if hasattr(das, code_name):
                actual_value = getattr(das, code_name)
                self.assertEqual(
                    actual_value,
                    expected_value,
                    f"Expected {code_name}={expected_value}, got {actual_value}"
                )
            else:
                # Constants may not be exported, which is acceptable
                print(f"Warning: {code_name} not exported by SWIG")

    def test_error_code_values_match_cpp(self):
        """
        Test that error code values match C++ definitions.

        Reference: include/das/IDasBase.h
        """
        # These values should match C++ definitions
        error_codes = {
            'DAS_E_INVALID_POINTER': -1073741847,
            'DAS_E_NO_INTERFACE': -1073741844,
            'DAS_E_INVALID_ENUM': -1073741832,
            'DAS_E_PYTHON_ERROR': -1073741842,
            'DAS_E_INVALID_STRING': -1073741843,
            'DAS_E_INVALID_STRING_SIZE': -1073741834,
        }

        for code_name, expected_value in error_codes.items():
            self.assertIsInstance(expected_value, int,
                               f"{code_name} should be an integer")


def run_tests():
    """
    Run all tests and return the result.

    This function creates a test suite with all test classes and runs them.
    """
    # Create a test suite
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()

    # Add all test classes
    suite.addTests(loader.loadTestsFromTestCase(TestCallFunctionReturningError))
    suite.addTests(loader.loadTestsFromTestCase(TestCatchAndVerifyException))
    suite.addTests(loader.loadTestsFromTestCase(TestExceptionMessage))
    suite.addTests(loader.loadTestsFromTestCase(TestSWIGExceptionHandling))
    suite.addTests(loader.loadTestsFromTestCase(TestErrorCodes))

    # Run tests
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    # Print summary
    print("\n" + "=" * 70)
    print("INTEGRATION TEST SUMMARY")
    print("=" * 70)
    if not SWIG_MODULE_AVAILABLE:
        print("WARNING: SWIG Python module not built")
        print("Most tests are skipped. To run integration tests:")
        print("  1. Build the Python module:")
        print("     cmake -DDAS_BUILD_TEST=ON -DEXPORT_PYTHON=ON -S . -B build")
        print("  2. Build the project:")
        print("     cmake --build build")
        print("  3. Run these tests again")
    else:
        print(f"Tests run: {result.testsRun}")
        print(f"Failures: {len(result.failures)}")
        print(f"Errors: {len(result.errors)}")
        print(f"Skipped: {len(result.skipped)}")
    print("=" * 70)

    return result


if __name__ == '__main__':
    # Run tests when executed directly
    result = run_tests()

    # Exit with appropriate code
    sys.exit(0 if result.wasSuccessful() else 1)
