if(NOT TARGET DasCoreCSharpExport)
    message(FATAL_ERROR "CSharpTestPlugin requires DasCoreCSharpExport from EXPORT_CSHARP=ON.")
endif()

if(NOT DEFINED DAS_CORE_CSHARP_PROJECT_FILE)
    message(FATAL_ERROR "CSharpTestPlugin requires DAS_CORE_CSHARP_PROJECT_FILE from DasCore C# export.")
endif()

set(CSHARP_TEST_PLUGIN_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/CSharpTestPlugin.csproj")
set(CSHARP_TEST_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/Plugins/CSharpTestPlugin")
set(CSHARP_TEST_PLUGIN_CONFIGURATION "$<CONFIG>")

function(das_add_csharp_test_plugin_build TARGET_NAME TARGET_FRAMEWORK)
    add_custom_target(${TARGET_NAME}
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CSHARP_TEST_PLUGIN_OUTPUT_DIR}/${TARGET_FRAMEWORK}/${CSHARP_TEST_PLUGIN_CONFIGURATION}"
        COMMAND "${DotNet_EXECUTABLE}" build "${CSHARP_TEST_PLUGIN_PROJECT}"
            --framework "${TARGET_FRAMEWORK}"
            --configuration "${CSHARP_TEST_PLUGIN_CONFIGURATION}"
            --output "${CSHARP_TEST_PLUGIN_OUTPUT_DIR}/${TARGET_FRAMEWORK}/${CSHARP_TEST_PLUGIN_CONFIGURATION}"
            -p:DasGeneratedProject="${DAS_CORE_CSHARP_PROJECT_FILE}"
            -p:DasGeneratedOutputDir="${DAS_CORE_CSHARP_OUTPUT_DIR}"
            -p:TargetFrameworks="${TARGET_FRAMEWORK}"
            -v:minimal
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "${CMAKE_CURRENT_SOURCE_DIR}/bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/obj"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM
    )
    add_dependencies(${TARGET_NAME} DasCoreCSharpExport)
endfunction()

if(DAS_CSHARP_BUILD_MODERN)
    das_add_csharp_test_plugin_build(CSharpTestPluginModernBuild net8.0)
    add_dependencies(CSharpTestPluginModernBuild DasCoreCSharpExport)
endif()

if(DAS_CSHARP_BUILD_NET48)
    das_add_csharp_test_plugin_build(CSharpTestPluginNet48Build net48)
    add_dependencies(CSharpTestPluginNet48Build DasCoreCSharpExport)
endif()

if(DAS_CSHARP_NATIVEAOT_SMOKE)
    set(CSHARP_TEST_PLUGIN_NATIVEAOT_RUNTIME_ID win-x64 CACHE STRING "Runtime identifier for the C# NativeAOT smoke publish")
    add_custom_target(CSharpTestPluginNativeAotPublish
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CSHARP_TEST_PLUGIN_OUTPUT_DIR}/nativeaot/${CSHARP_TEST_PLUGIN_CONFIGURATION}"
        COMMAND "${DotNet_EXECUTABLE}" publish "${CSHARP_TEST_PLUGIN_PROJECT}"
            --framework net8.0
            --configuration "${CSHARP_TEST_PLUGIN_CONFIGURATION}"
            --runtime "${CSHARP_TEST_PLUGIN_NATIVEAOT_RUNTIME_ID}"
            --self-contained true
            --output "${CSHARP_TEST_PLUGIN_OUTPUT_DIR}/nativeaot/${CSHARP_TEST_PLUGIN_CONFIGURATION}"
            -p:PublishAot=true
            -p:DasGeneratedProject="${DAS_CORE_CSHARP_PROJECT_FILE}"
            -p:DasGeneratedOutputDir="${DAS_CORE_CSHARP_OUTPUT_DIR}"
            -p:TargetFrameworks=net8.0
            -v:minimal
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "${CMAKE_CURRENT_SOURCE_DIR}/bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/obj"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM
    )
    add_dependencies(CSharpTestPluginNativeAotPublish DasCoreCSharpExport)
endif()
