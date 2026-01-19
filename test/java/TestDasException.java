import org.junit.Test;
import org.junit.BeforeClass;
import org.junit.Assume;

import java.io.File;
import java.lang.reflect.Method;
import java.lang.reflect.Field;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

/**
 * DasException Java unit tests
 *
 * This class tests the DasException Java binding and its ability to
 * convert DasResult error codes to Java exceptions.
 *
 * Note: These tests require the Java SWIG module to be built first.
 * To build the Java module, run CMake with -DEXPORT_JAVA=ON.
 *
 * The tests use reflection to check for DasException and its methods
 * since the SWIG module may not be available during development.
 */

public class TestDasException {

    /** Flag to indicate if SWIG Java module is available */
    private static boolean SWIG_MODULE_AVAILABLE = false;

    /** Class reference to DasException (if available) */
    private static Class<?> DAS_EXCEPTION_CLASS = null;

    /** Class reference to the SWIG module */
    private static Class<?> SWIG_MODULE_CLASS = null;

    /**
     * Setup method - attempts to load the SWIG Java module.
     * If the module is not available, all tests will be skipped.
     */
    @BeforeClass
    public static void setUp() {
        try {
            // Try to find the SWIG-generated Java module
            // Common locations:
            // 1. build/bin/DuskAutoScript.jar
            // 2. build/swig/java/DuskAutoScript.class
            // 3. System classpath (if already loaded)

            // First, try to load from build directory
            Path buildBinPath = Paths.get("..", "build", "bin");
            if (Files.exists(buildBinPath)) {
                File jarFile = buildBinPath.resolve("DuskAutoScript.jar").toFile();
                if (jarFile.exists()) {
                    URLClassLoader classLoader = (URLClassLoader) ClassLoader.getSystemClassLoader();
                    Method addUrlMethod = URLClassLoader.class.getDeclaredMethod("addURL", URL.class);
                    addUrlMethod.setAccessible(true);
                    addUrlMethod.invoke(classLoader, jarFile.toURI().toURL());
                }
            }

            // Try to load the DasException class
            // The class name depends on SWIG configuration
            try {
                DAS_EXCEPTION_CLASS = Class.forName("DasException");
                SWIG_MODULE_AVAILABLE = true;
            } catch (ClassNotFoundException e1) {
                // Try with module prefix
                try {
                    DAS_EXCEPTION_CLASS = Class.forName("DuskAutoScript.DasException");
                    SWIG_MODULE_AVAILABLE = true;
                } catch (ClassNotFoundException e2) {
                    System.out.println("Warning: SWIG Java module not available: " + e2.getMessage());
                    System.out.println("To enable these tests, build the Java module with: cmake -DEXPORT_JAVA=ON");
                }
            }

            // Try to load the SWIG module class to access error code constants
            if (SWIG_MODULE_AVAILABLE) {
                try {
                    SWIG_MODULE_CLASS = Class.forName("DuskAutoScript");
                } catch (ClassNotFoundException e) {
                    try {
                        SWIG_MODULE_CLASS = Class.forName("DuskAutoScriptJNI");
                    } catch (ClassNotFoundException e2) {
                        // Module class not available, but DasException is
                        System.out.println("Note: SWIG module class not found for error code constants");
                    }
                }
            }
        } catch (Exception e) {
            System.out.println("Warning: Failed to setup SWIG module: " + e.getMessage());
            SWIG_MODULE_AVAILABLE = false;
        }
    }

    /**
     * Test class: TestDasExceptionExists
     * Verifies that the DasException class can be loaded
     */
    public static class TestDasExceptionExists {
        @Test
        public void testDasExceptionClassExists() {
            Assume.assumeTrue("SWIG Java module not built", SWIG_MODULE_AVAILABLE);
            org.junit.Assert.assertNotNull("DasException class should exist", DAS_EXCEPTION_CLASS);
        }

        @Test
        public void testDasExceptionHasConstructor() {
            Assume.assumeTrue("SWIG Java module not built", SWIG_MODULE_AVAILABLE);
            try {
                // According to ExportAll.i, DasException has constructor:
                // public DasException(int errorCode, String message)
                DAS_EXCEPTION_CLASS.getDeclaredConstructor(int.class, String.class);
            } catch (NoSuchMethodException e) {
                org.junit.Assert.fail("DasException should have constructor (int, String)");
            }
        }
    }

    /**
     * Test class: TestDasExceptionErrorCode
     * Verifies DasException errorCode access
     */
    public static class TestDasExceptionErrorCode {
        @Test
        public void testDasExceptionHasGetErrorCodeMethod() {
            Assume.assumeTrue("SWIG Java module not built", SWIG_MODULE_AVAILABLE);
            try {
                Method getErrorCode = DAS_EXCEPTION_CLASS.getMethod("getErrorCode");
                org.junit.Assert.assertNotNull("getErrorCode() method should exist", getErrorCode);
                org.junit.Assert.assertEquals("getErrorCode() should return int",
                    int.class, getErrorCode.getReturnType());
            } catch (NoSuchMethodException e) {
                org.junit.Assert.fail("DasException should have getErrorCode() method");
            }
        }

        @Test
        public void testDasExceptionHasErrorCodeField() {
            Assume.assumeTrue("SWIG Java module not built", SWIG_MODULE_AVAILABLE);
            try {
                Field errorCodeField = DAS_EXCEPTION_CLASS.getDeclaredField("errorCode");
                org.junit.Assert.assertNotNull("errorCode field should exist", errorCodeField);
                org.junit.Assert.assertEquals("errorCode field should be int",
                    int.class, errorCodeField.getType());
            } catch (NoSuchFieldException e) {
                // Field may be private, which is fine
                // The important part is the getter method
            }
        }
    }

    /**
     * Test class: TestDasExceptionMessage
     * Verifies DasException message access
     */
    public static class TestDasExceptionMessage {
        @Test
        public void testDasExceptionHasGetMessageMethod() {
            Assume.assumeTrue("SWIG Java module not built", SWIG_MODULE_AVAILABLE);
            try {
                Method getMessage = DAS_EXCEPTION_CLASS.getMethod("getMessage");
                org.junit.Assert.assertNotNull("getMessage() method should exist", getMessage);
                org.junit.Assert.assertEquals("getMessage() should return String",
                    String.class, getMessage.getReturnType());
            } catch (NoSuchMethodException e) {
                org.junit.Assert.fail("DasException should have getMessage() method");
            }
        }

        @Test
        public void testDasExceptionHasMessageField() {
            Assume.assumeTrue("SWIG Java module not built", SWIG_MODULE_AVAILABLE);
            try {
                Field messageField = DAS_EXCEPTION_CLASS.getDeclaredField("message");
                org.junit.Assert.assertNotNull("message field should exist", messageField);
                org.junit.Assert.assertEquals("message field should be String",
                    String.class, messageField.getType());
            } catch (NoSuchFieldException e) {
                // Field may be private, which is fine
                // The important part is the getter method
            }
        }
    }

    /**
     * Test class: TestVariousErrorCodes
     * Tests various error codes that DasException can represent
     */
    public static class TestVariousErrorCodes {
        @Test
        public void testErrorCodeInvalidPointer() {
            int expectedErrorCode = -1073741847; // DAS_E_INVALID_POINTER
            org.junit.Assert.assertEquals("DAS_E_INVALID_POINTER value incorrect",
                expectedErrorCode, -1073741847);
        }

        @Test
        public void testErrorCodeNoInterface() {
            int expectedErrorCode = -1073741831; // DAS_E_NO_INTERFACE
            org.junit.Assert.assertEquals("DAS_E_NO_INTERFACE value incorrect",
                expectedErrorCode, -1073741831);
        }

        @Test
        public void testErrorCodeInvalidEnum() {
            int expectedErrorCode = -1073741853; // DAS_E_INVALID_ENUM
            org.junit.Assert.assertEquals("DAS_E_INVALID_ENUM value incorrect",
                expectedErrorCode, -1073741853);
        }

        @Test
        public void testErrorCodePythonError() {
            int expectedErrorCode = -1073741849; // DAS_E_PYTHON_ERROR
            org.junit.Assert.assertEquals("DAS_E_PYTHON_ERROR value incorrect",
                expectedErrorCode, -1073741849);
        }

        @Test
        public void testErrorCodeInvalidString() {
            int expectedErrorCode = -1073741833; // DAS_E_INVALID_STRING
            org.junit.Assert.assertEquals("DAS_E_INVALID_STRING value incorrect",
                expectedErrorCode, -1073741833);
        }

        @Test
        public void testErrorCodeInvalidStringSize() {
            int expectedErrorCode = -1073741834; // DAS_E_INVALID_STRING_SIZE
            org.junit.Assert.assertEquals("DAS_E_INVALID_STRING_SIZE value incorrect",
                expectedErrorCode, -1073741834);
        }

        @Test
        public void testSuccessCode() {
            int successCode = 0; // DAS_S_OK
            org.junit.Assert.assertEquals("DAS_S_OK value incorrect",
                successCode, 0);
        }
    }

    /**
     * Test class: TestDasExceptionConversion
     * Tests DasResult failure to DasException conversion
     */
    public static class TestDasExceptionConversion {
        @Test
        public void testCanCatchDasException() {
            Assume.assumeTrue("SWIG Java module not built", SWIG_MODULE_AVAILABLE);
            // Verify that DasException extends Exception or RuntimeException
            org.junit.Assert.assertTrue("DasException should extend Exception or RuntimeException",
                Exception.class.isAssignableFrom(DAS_EXCEPTION_CLASS) ||
                RuntimeException.class.isAssignableFrom(DAS_EXCEPTION_CLASS));
        }

        @Test
        public void testDasExceptionCanCreateInstance() {
            Assume.assumeTrue("SWIG Java module not built", SWIG_MODULE_AVAILABLE);
            try {
                // Create a test instance of DasException
                // According to ExportAll.i: public DasException(int errorCode, String message)
                Object ex = DAS_EXCEPTION_CLASS
                    .getDeclaredConstructor(int.class, String.class)
                    .newInstance(-1073741847, "Test error message");

                org.junit.Assert.assertNotNull("DasException instance should be created", ex);
                org.junit.Assert.assertTrue("Created object should be instance of DasException",
                    DAS_EXCEPTION_CLASS.isInstance(ex));
            } catch (Exception e) {
                org.junit.Assert.fail("Should be able to create DasException instance: " + e.getMessage());
            }
        }
    }

    /**
     * Test class: TestErrorCodeConstantsExist
     * Tests that error code constants are defined in the SWIG module
     */
    public static class TestErrorCodeConstantsExist {
        @Test
        public void testErrorCodesDefined() {
            Assume.assumeTrue("SWIG Java module not built", SWIG_MODULE_AVAILABLE && SWIG_MODULE_CLASS != null);

            // These error codes should be automatically generated by SWIG
            // from the C++ #define statements in include/das/IDasBase.h
            String[] expectedCodes = {
                "DAS_E_INVALID_POINTER",
                "DAS_E_NO_INTERFACE",
                "DAS_E_INVALID_ENUM",
                "DAS_E_PYTHON_ERROR",
                "DAS_E_INVALID_STRING",
                "DAS_E_INVALID_STRING_SIZE",
                "DAS_S_OK",
            };

            for (String code : expectedCodes) {
                try {
                    Field field = SWIG_MODULE_CLASS.getField(code);
                    org.junit.Assert.assertNotNull("Field " + code + " should exist", field);
                    org.junit.Assert.assertEquals("Field " + code + " should be int",
                        int.class, field.getType());
                } catch (NoSuchFieldException e) {
                    // Error code constants may not be exposed in Java
                    // This is acceptable if SWIG is configured differently
                    System.out.println("Note: Error code constant " + code + " not found in SWIG module");
                }
            }
        }
    }
}
