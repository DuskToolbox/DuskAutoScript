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

if(NOT TARGET DasCoreNodePackageCopy)
    message(FATAL_ERROR "[NodeTestPlugin] DasCoreNodePackageCopy target is required")
endif()

if(NOT NODE_ADDON_NAME)
    message(FATAL_ERROR "[NodeTestPlugin] NODE_ADDON_NAME is required")
endif()

set(PLUGIN_NAME "NodeTestPlugin")
set(FOLDER_PLUGIN_NAME "NodeFolderTestPlugin")

if(CMAKE_CONFIGURATION_TYPES)
    set(_NODE_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/plugins")
else()
    set(_NODE_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins")
endif()

set(_NODE_FOLDER_PLUGIN_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/${FOLDER_PLUGIN_NAME}")
set(_NODE_FOLDER_PLUGIN_OUTPUT_DIR
    "${_NODE_PLUGIN_OUTPUT_DIR}/${FOLDER_PLUGIN_NAME}")

set(_NODE_PLUGIN_SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.cjs"
    "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
)

set(_NODE_FOLDER_PLUGIN_SOURCE_FILES
    "${_NODE_FOLDER_PLUGIN_SOURCE_DIR}/${FOLDER_PLUGIN_NAME}.json"
    "${_NODE_FOLDER_PLUGIN_SOURCE_DIR}/package.json"
)

add_custom_target(${PLUGIN_NAME}Copy ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${_NODE_PLUGIN_OUTPUT_DIR}"
        "${_NODE_FOLDER_PLUGIN_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "${_NODE_PLUGIN_OUTPUT_DIR}/das-node-host.cjs"
        "${_NODE_PLUGIN_OUTPUT_DIR}/${NODE_ADDON_NAME}_export.js"
        "${_NODE_PLUGIN_OUTPUT_DIR}/${NODE_ADDON_NAME}_export.d.ts"
        "${_NODE_PLUGIN_OUTPUT_DIR}/${NODE_ADDON_NAME}.node"
        "${_NODE_PLUGIN_OUTPUT_DIR}/$<TARGET_FILE_NAME:DasCore>"
        "${_NODE_FOLDER_PLUGIN_OUTPUT_DIR}/$<TARGET_FILE_NAME:DasCore>"
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
        "${_NODE_FOLDER_PLUGIN_SOURCE_DIR}/${FOLDER_PLUGIN_NAME}.json"
        "${_NODE_FOLDER_PLUGIN_OUTPUT_DIR}/${FOLDER_PLUGIN_NAME}.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_NODE_FOLDER_PLUGIN_SOURCE_DIR}/package.json"
        "${_NODE_FOLDER_PLUGIN_OUTPUT_DIR}/package.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.cjs"
        "${_NODE_FOLDER_PLUGIN_OUTPUT_DIR}/index.cjs"
    DEPENDS
        ${_NODE_PLUGIN_SOURCE_FILES}
        ${_NODE_FOLDER_PLUGIN_SOURCE_FILES}
    COMMENT "[NodeTestPlugin] Copying normalized plugin packages to output directory"
    COMMAND_EXPAND_LISTS
    VERBATIM
)

add_dependencies(${PLUGIN_NAME}Copy DasCoreNodeExport DasCoreNodePackageCopy)

message(STATUS "[NodeTestPlugin] Output: ${_NODE_PLUGIN_OUTPUT_DIR}")
