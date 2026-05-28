# das/Plugins/PythonTestPlugin/CMakeLists.txt
# Python test plugin package copy wiring.

if(NOT DAS_BUILD_TEST)
    message(STATUS "[PythonTestPlugin] DAS_BUILD_TEST is OFF, skipping")
    return()
endif()

if(NOT EXPORT_PYTHON)
    message(STATUS "[PythonTestPlugin] EXPORT_PYTHON is OFF, skipping")
    return()
endif()

if(NOT TARGET DasCorePythonExport)
    message(FATAL_ERROR "[PythonTestPlugin] DasCorePythonExport target is required")
endif()

set(PLUGIN_NAME "PythonTestPlugin")

if(CMAKE_CONFIGURATION_TYPES)
    set(_PYTHON_PLUGIN_OUTPUT_DIR
        "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/plugins/${PLUGIN_NAME}")
else()
    set(_PYTHON_PLUGIN_OUTPUT_DIR
        "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins/${PLUGIN_NAME}")
endif()

set(_PYTHON_PLUGIN_SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/python_test_plugin.py"
)

add_custom_target(${PLUGIN_NAME}Copy ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${_PYTHON_PLUGIN_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
        "${_PYTHON_PLUGIN_OUTPUT_DIR}/manifest.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/python_test_plugin.py"
        "${_PYTHON_PLUGIN_OUTPUT_DIR}/python_test_plugin.py"
    DEPENDS
        ${_PYTHON_PLUGIN_SOURCE_FILES}
    COMMENT "[PythonTestPlugin] Copying plugin package to output directory"
    COMMAND_EXPAND_LISTS
    VERBATIM
)

add_dependencies(${PLUGIN_NAME}Copy DasCorePythonExport)

message(STATUS "[PythonTestPlugin] Output: ${_PYTHON_PLUGIN_OUTPUT_DIR}")
