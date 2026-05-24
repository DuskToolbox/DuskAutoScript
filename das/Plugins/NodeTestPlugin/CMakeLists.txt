# das/Plugins/NodeTestPlugin/CMakeLists.txt
# Node test plugin package copy wiring.

if(NOT DAS_BUILD_TEST)
    message(STATUS "[NodeTestPlugin] DAS_BUILD_TEST is OFF, skipping")
    return()
endif()

if(NOT EXPORT_NODE)
    message(STATUS "[NodeTestPlugin] EXPORT_NODE is OFF, skipping")
    return()
endif()

if(NOT TARGET DasCoreNodeExport)
    message(FATAL_ERROR "[NodeTestPlugin] DasCoreNodeExport target is required")
endif()

if(NOT NODE_ADDON_NAME)
    message(FATAL_ERROR "[NodeTestPlugin] NODE_ADDON_NAME is required")
endif()

set(PLUGIN_NAME "NodeTestPlugin")

if(CMAKE_CONFIGURATION_TYPES)
    set(_NODE_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/plugins")
else()
    set(_NODE_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins")
endif()

set(_NODE_EXPORT_DIR "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/node")
set(_NODE_HOST_SCRIPT "${CMAKE_SOURCE_DIR}/tools/das_idl/node_host/das-node-host.cjs")
set(_NODE_WRAPPER_SCRIPT "${_NODE_EXPORT_DIR}/${NODE_ADDON_NAME}_export.js")

set(_NODE_PLUGIN_SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.cjs"
    "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
)

add_custom_target(${PLUGIN_NAME}Copy ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_NODE_PLUGIN_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.json"
        "${_NODE_PLUGIN_OUTPUT_DIR}/${PLUGIN_NAME}.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.cjs"
        "${_NODE_PLUGIN_OUTPUT_DIR}/${PLUGIN_NAME}.cjs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
        "${_NODE_PLUGIN_OUTPUT_DIR}/package.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_NODE_HOST_SCRIPT}"
        "${_NODE_PLUGIN_OUTPUT_DIR}/das-node-host.cjs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_NODE_WRAPPER_SCRIPT}"
        "${_NODE_PLUGIN_OUTPUT_DIR}/${NODE_ADDON_NAME}_export.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:DasCoreNodeExport>"
        "${_NODE_PLUGIN_OUTPUT_DIR}/${NODE_ADDON_NAME}.node"
    DEPENDS
        ${_NODE_PLUGIN_SOURCE_FILES}
        "${_NODE_HOST_SCRIPT}"
        "${_NODE_WRAPPER_SCRIPT}"
    COMMENT "[NodeTestPlugin] Copying plugin files to output directory"
    VERBATIM
)

add_dependencies(${PLUGIN_NAME}Copy DasCoreNodeExport)

message(STATUS "[NodeTestPlugin] Output: ${_NODE_PLUGIN_OUTPUT_DIR}")
