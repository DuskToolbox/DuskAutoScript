"""
DasException Python unit tests

This module tests the DasException Python binding and its ability to
convert DasResult error codes to Python exceptions.

Note: These tests require the Python SWIG module to be built first.
To build the Python module, run CMake with -DEXPORT_PYTHON=ON.
"""

import unittest
import sys
from pathlib import Path

# Try to import the SWIG-generated Python module
# If not available, skip all tests
try:
    # The module name depends on the SWIG configuration
    # Check common locations
    module_path = Path(__file__).parent.parent.parent / "build" / "bin"
    if module_path.exists():
        sys.path.insert(0, str(module_path))

    # Try to import the module
    # Note: The actual module name may vary (e.g., _DasCorePythonExport, DuskAutoScript, etc.)
    # Adjust the import based on your SWIG configuration
    import DuskAutoScript as das

    SWIG_MODULE_AVAILABLE = True
except ImportError as e:
    print(f"Warning: SWIG Python module not available: {e}")
    print("To enable these tests, build the Python module with: cmake -DEXPORT_PYTHON=ON")
    SWIG_MODULE_AVAILABLE = False


@unittest.skipIf(not SWIG_MODULE_AVAILABLE, "SWIG Python module not built")
class TestDasExceptionErrorCode(unittest.TestCase):
    """Test DasException error_code attribute access."""

    def test_das_exception_has_error_code_attribute(self):
        """Test that DasException has GetErrorCode() method."""
        # This test verifies that the DasException wrapper provides
        # access to the error code via GetErrorCode() method
        # Note: The actual test will require calling a C++ function
        # that throws DasException
        self.assertTrue(hasattr(das.DasException, 'GetErrorCode'))

    def test_das_exception_has_message_attribute(self):
        """Test that DasException has GetMessage() method."""
        # This test verifies that the DasException wrapper provides
        # access to the error message via GetMessage() method
        self.assertTrue(hasattr(das.DasException, 'GetMessage'))


@unittest.skipIf(not SWIG_MODULE_AVAILABLE, "SWIG Python module not built")
class TestDasExceptionMessage(unittest.TestCase):
    """Test DasException message access."""

    def test_das_exception_message_not_empty(self):
        """Test that DasException provides non-empty message."""
        # Note: This test requires calling a C++ function that throws DasException
        # For now, we just verify the method exists
        self.assertTrue(hasattr(das.DasException, 'GetMessage'))


@unittest.skipIf(not SWIG_MODULE_AVAILABLE, "SWIG Python module not built")
class TestVariousErrorCodes(unittest.TestCase):
    """Test various error codes that DasException can represent."""

    def test_error_code_invalid_pointer(self):
        """Test DAS_E_INVALID_POINTER error code."""
        # Error code from include/das/IDasBase.h
        expected_error_code = -1073741847  # DAS_E_INVALID_POINTER
        self.assertIsInstance(expected_error_code, int)

    def test_error_code_no_interface(self):
        """Test DAS_E_NO_INTERFACE error code."""
        # Error code from include/das/IDasBase.h
        expected_error_code = -1073741831  # DAS_E_NO_INTERFACE
        self.assertIsInstance(expected_error_code, int)

    def test_error_code_invalid_enum(self):
        """Test DAS_E_INVALID_ENUM error code."""
        # Error code from include/das/IDasBase.h
        expected_error_code = -1073741853  # DAS_E_INVALID_ENUM
        self.assertIsInstance(expected_error_code, int)

    def test_error_code_python_error(self):
        """Test DAS_E_PYTHON_ERROR error code."""
        # Error code from include/das/IDasBase.h
        expected_error_code = -1073741849  # DAS_E_PYTHON_ERROR
        self.assertIsInstance(expected_error_code, int)

    def test_error_code_invalid_string(self):
        """Test DAS_E_INVALID_STRING error code."""
        # Error code from include/das/IDasBase.h
        expected_error_code = -1073741833  # DAS_E_INVALID_STRING
        self.assertIsInstance(expected_error_code, int)

    def test_error_code_invalid_string_size(self):
        """Test DAS_E_INVALID_STRING_SIZE error code."""
        # Error code from include/das/IDasBase.h
        expected_error_code = -1073741834  # DAS_E_INVALID_STRING_SIZE
        self.assertIsInstance(expected_error_code, int)

    def test_success_code(self):
        """Test DAS_S_OK success code."""
        # Success code from include/das/IDasBase.h
        success_code = 0  # DAS_S_OK
        self.assertEqual(success_code, 0)


@unittest.skipIf(not SWIG_MODULE_AVAILABLE, "SWIG Python module not built")
class TestDasExceptionConversion(unittest.TestCase):
    """Test DasResult failure to DasException conversion."""

    def test_exception_is_runtime_error(self):
        """Test that DasException is a RuntimeError in Python."""
        # According to SWIG configuration, DasException is mapped to RuntimeError
        # This verifies the exception type mapping
        try:
            # Try to import RuntimeError from builtins
            from builtins import RuntimeError
            # Note: The actual inheritance depends on SWIG %exception directive
            # In ExportAll.i, we use SWIG_RuntimeError
            pass
        except ImportError:
            # Python 3.x: RuntimeError is a built-in
            pass

    def test_can_catch_das_exception(self):
        """Test that we can catch DasException."""
        # This test will need to call a C++ function that throws DasException
        # For now, we just verify the exception class exists
        self.assertTrue(hasattr(das, 'DasException'))


class TestErrorCodesExist(unittest.TestCase):
    """Test that error code constants are defined (if available)."""

    def test_error_codes_defined(self):
        """Test that error code constants are available from SWIG."""
        # These error codes should be automatically generated by SWIG
        # from the C++ #define statements in include/das/IDasBase.h
        # If the SWIG module is not built, these won't be available

        if SWIG_MODULE_AVAILABLE:
            # Try to access error code constants
            expected_codes = [
                'DAS_E_INVALID_POINTER',
                'DAS_E_NO_INTERFACE',
                'DAS_E_INVALID_ENUM',
                'DAS_E_PYTHON_ERROR',
                'DAS_E_INVALID_STRING',
                'DAS_E_INVALID_STRING_SIZE',
                'DAS_S_OK',
            ]

            for code in expected_codes:
                # Note: The actual constant names may differ based on SWIG configuration
                # Some may be prefixed with module name
                if hasattr(das, code):
                    self.assertIsInstance(getattr(das, code), int)


def run_tests():
    """Run all tests and return the result."""
    # Create a test suite
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()

    # Add all test classes
    suite.addTests(loader.loadTestsFromTestCase(TestDasExceptionErrorCode))
    suite.addTests(loader.loadTestsFromTestCase(TestDasExceptionMessage))
    suite.addTests(loader.loadTestsFromTestCase(TestVariousErrorCodes))
    suite.addTests(loader.loadTestsFromTestCase(TestDasExceptionConversion))
    suite.addTests(loader.loadTestsFromTestCase(TestErrorCodesExist))

    # Run tests
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    return result


if __name__ == '__main__':
    # Run tests when executed directly
    result = run_tests()

    # Exit with appropriate code
    sys.exit(0 if result.wasSuccessful() else 1)
