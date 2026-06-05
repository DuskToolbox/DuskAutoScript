if(NOT TARGET DasCoreCSharpExport)
    message(FATAL_ERROR "CSharpTestPlugin requires DasCoreCSharpExport from EXPORT_CSHARP=ON.")
endif()

if(NOT DEFINED DAS_CORE_CSHARP_PROJECT_FILE)
    message(FATAL_ERROR "CSharpTestPlugin requires DAS_CORE_CSHARP_PROJECT_FILE from DasCore C# export.")
endif()

set(CSHARP_TEST_PLUGIN_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/CSharpTestPlugin.csproj")
set(CSHARP_TEST_PLUGIN_BINARY_ROOT "${CMAKE_BINARY_DIR}/das/Plugins/CSharpTestPlugin")
set(CSHARP_TEST_PLUGIN_BUILD_ROOT "${CSHARP_TEST_PLUGIN_BINARY_ROOT}/build")
set(CSHARP_TEST_PLUGIN_PACKAGE_ROOT "${CSHARP_TEST_PLUGIN_BINARY_ROOT}/packages")
set(CSHARP_TEST_PLUGIN_MANIFEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/manifests")
set(CSHARP_TEST_PLUGIN_CONFIGURATION "$<CONFIG>")
set(CSHARP_TEST_PLUGIN_ENTRY_POINT "Das.TestPlugin.Bootstrap.Create")
set(CSHARP_TEST_PLUGIN_EXTENSION "dll")

set(CSHARP_TEST_PLUGIN_MODERN_NAME "DasCSharpTestPluginModern")
set(CSHARP_TEST_PLUGIN_MODERN_GUID "78D6C0A1-0008-4000-8000-000000000078")
set(CSHARP_TEST_PLUGIN_MODERN_TFM "net8.0")
set(CSHARP_TEST_PLUGIN_MODERN_RUNTIME_CONFIG
    "${CSHARP_TEST_PLUGIN_MODERN_NAME}.runtimeconfig.json")

set(CSHARP_TEST_PLUGIN_NET48_NAME "DasCSharpTestPluginNet48")
set(CSHARP_TEST_PLUGIN_NET48_GUID "78D6C0A1-0048-4000-8000-000000000078")
set(CSHARP_TEST_PLUGIN_NET48_TFM "net48")

set(CSHARP_TEST_PLUGIN_PACKAGE_NAME "${CSHARP_TEST_PLUGIN_MODERN_NAME}")
set(CSHARP_TEST_PLUGIN_PACKAGE_GUID "${CSHARP_TEST_PLUGIN_MODERN_GUID}")
set(CSHARP_TEST_PLUGIN_TARGET_FRAMEWORK "${CSHARP_TEST_PLUGIN_MODERN_TFM}")
set(CSHARP_TEST_PLUGIN_RUNTIME_CONFIG_PATH
    "${CSHARP_TEST_PLUGIN_MODERN_RUNTIME_CONFIG}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/CSharpTestPlugin.net8.json.in"
    "${CSHARP_TEST_PLUGIN_MANIFEST_ROOT}/CSharpTestPlugin.net8.json"
    @ONLY)

set(CSHARP_TEST_PLUGIN_PACKAGE_NAME "${CSHARP_TEST_PLUGIN_NET48_NAME}")
set(CSHARP_TEST_PLUGIN_PACKAGE_GUID "${CSHARP_TEST_PLUGIN_NET48_GUID}")
set(CSHARP_TEST_PLUGIN_TARGET_FRAMEWORK "${CSHARP_TEST_PLUGIN_NET48_TFM}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/CSharpTestPlugin.net48.json.in"
    "${CSHARP_TEST_PLUGIN_MANIFEST_ROOT}/CSharpTestPlugin.net48.json"
    @ONLY)

function(das_add_csharp_test_plugin_build
    TARGET_NAME
    TARGET_FRAMEWORK
    PACKAGE_NAME)
    set(_BUILD_OUTPUT_DIR
        "${CSHARP_TEST_PLUGIN_BUILD_ROOT}/${TARGET_FRAMEWORK}/${CSHARP_TEST_PLUGIN_CONFIGURATION}")

    add_custom_target(${TARGET_NAME}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_BUILD_OUTPUT_DIR}"
        COMMAND "${DotNet_EXECUTABLE}" build "${CSHARP_TEST_PLUGIN_PROJECT}"
            --framework "${TARGET_FRAMEWORK}"
            --configuration "${CSHARP_TEST_PLUGIN_CONFIGURATION}"
            --output "${_BUILD_OUTPUT_DIR}"
            -p:DasPluginAssemblyName="${PACKAGE_NAME}"
            -p:GenerateRuntimeConfigurationFiles=true
            -p:DasGeneratedProject="${DAS_CORE_CSHARP_PROJECT_FILE}"
            -p:DasGeneratedOutputDir="${DAS_CORE_CSHARP_OUTPUT_DIR}"
            -p:TargetFrameworks="${TARGET_FRAMEWORK}"
            -v:minimal
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "${CMAKE_CURRENT_SOURCE_DIR}/bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/obj"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM)
    add_dependencies(${TARGET_NAME} DasCoreCSharpExport)
endfunction()

function(das_add_csharp_test_plugin_package
    TARGET_NAME
    BUILD_TARGET
    TARGET_FRAMEWORK
    PACKAGE_NAME
    MANIFEST_FILE
    PACKAGE_SUBDIR)
    set(_BUILD_OUTPUT_DIR
        "${CSHARP_TEST_PLUGIN_BUILD_ROOT}/${TARGET_FRAMEWORK}/${CSHARP_TEST_PLUGIN_CONFIGURATION}")
    set(_PACKAGE_OUTPUT_DIR
        "${CSHARP_TEST_PLUGIN_PACKAGE_ROOT}/${PACKAGE_SUBDIR}/${CSHARP_TEST_PLUGIN_CONFIGURATION}")
    set(_PACKAGE_BINARY "${PACKAGE_NAME}.${CSHARP_TEST_PLUGIN_EXTENSION}")

    add_custom_target(${TARGET_NAME}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_PACKAGE_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CSHARP_TEST_PLUGIN_MANIFEST_ROOT}/${MANIFEST_FILE}"
            "${_PACKAGE_OUTPUT_DIR}/CSharpTestPlugin.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_BUILD_OUTPUT_DIR}/${_PACKAGE_BINARY}"
            "${_PACKAGE_OUTPUT_DIR}/${_PACKAGE_BINARY}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM)
    add_dependencies(${TARGET_NAME} ${BUILD_TARGET} DasCoreCSharpExport)
endfunction()

function(das_add_csharp_test_plugin_modern_package
    TARGET_NAME
    BUILD_TARGET
    TARGET_FRAMEWORK
    PACKAGE_NAME
    MANIFEST_FILE
    PACKAGE_SUBDIR)
    set(_BUILD_OUTPUT_DIR
        "${CSHARP_TEST_PLUGIN_BUILD_ROOT}/${TARGET_FRAMEWORK}/${CSHARP_TEST_PLUGIN_CONFIGURATION}")
    set(_PACKAGE_OUTPUT_DIR
        "${CSHARP_TEST_PLUGIN_PACKAGE_ROOT}/${PACKAGE_SUBDIR}/${CSHARP_TEST_PLUGIN_CONFIGURATION}")
    set(_PACKAGE_BINARY "${PACKAGE_NAME}.${CSHARP_TEST_PLUGIN_EXTENSION}")
    set(_RUNTIME_CONFIG "${PACKAGE_NAME}.runtimeconfig.json")
    set(_DEPS_JSON "${PACKAGE_NAME}.deps.json")

    add_custom_target(${TARGET_NAME}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_PACKAGE_OUTPUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CSHARP_TEST_PLUGIN_MANIFEST_ROOT}/${MANIFEST_FILE}"
            "${_PACKAGE_OUTPUT_DIR}/CSharpTestPlugin.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_BUILD_OUTPUT_DIR}/${_PACKAGE_BINARY}"
            "${_PACKAGE_OUTPUT_DIR}/${_PACKAGE_BINARY}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_BUILD_OUTPUT_DIR}/${_RUNTIME_CONFIG}"
            "${_PACKAGE_OUTPUT_DIR}/${_RUNTIME_CONFIG}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_BUILD_OUTPUT_DIR}/${_DEPS_JSON}"
            "${_PACKAGE_OUTPUT_DIR}/${_DEPS_JSON}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM)
    add_dependencies(${TARGET_NAME} ${BUILD_TARGET} DasCoreCSharpExport)
endfunction()

if(DAS_CSHARP_BUILD_MODERN)
    das_add_csharp_test_plugin_build(
        CSharpTestPluginModernBuild
        "${CSHARP_TEST_PLUGIN_MODERN_TFM}"
        "${CSHARP_TEST_PLUGIN_MODERN_NAME}")
    das_add_csharp_test_plugin_modern_package(
        CSharpTestPluginModernPackage
        CSharpTestPluginModernBuild
        "${CSHARP_TEST_PLUGIN_MODERN_TFM}"
        "${CSHARP_TEST_PLUGIN_MODERN_NAME}"
        "CSharpTestPlugin.net8.json"
        "modern")
endif()

set(CSHARP_TEST_PLUGIN_GENERATED_NATIVE_SUPPORT
    "${DAS_CORE_CSHARP_OUTPUT_DIR}/Native/DasCSharpDirectorSupport.cpp")
set_source_files_properties(
    "${CSHARP_TEST_PLUGIN_GENERATED_NATIVE_SUPPORT}"
    PROPERTIES GENERATED TRUE)
add_library(CSharpTestPluginGeneratedNativeSupportCompile OBJECT
    "${CSHARP_TEST_PLUGIN_GENERATED_NATIVE_SUPPORT}")
add_dependencies(CSharpTestPluginGeneratedNativeSupportCompile DasCoreCSharpExport)
target_include_directories(CSharpTestPluginGeneratedNativeSupportCompile PRIVATE
    "${DAS_CORE_CSHARP_OUTPUT_DIR}/Native"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/das/include"
    "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/abi"
    "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/header")
target_compile_definitions(CSharpTestPluginGeneratedNativeSupportCompile PRIVATE
    DAS_BUILD_SHARED)
if(MSVC)
    target_compile_options(CSharpTestPluginGeneratedNativeSupportCompile PRIVATE
        /bigobj)
endif()

if(DAS_CSHARP_BUILD_NET48)
    das_add_csharp_test_plugin_build(
        CSharpTestPluginNet48Build
        "${CSHARP_TEST_PLUGIN_NET48_TFM}"
        "${CSHARP_TEST_PLUGIN_NET48_NAME}")
    das_add_csharp_test_plugin_package(
        CSharpTestPluginNet48Package
        CSharpTestPluginNet48Build
        "${CSHARP_TEST_PLUGIN_NET48_TFM}"
        "${CSHARP_TEST_PLUGIN_NET48_NAME}"
        "CSharpTestPlugin.net48.json"
        "net48")
endif()

if(DAS_CSHARP_NATIVEAOT_SMOKE)
    set(CSHARP_TEST_PLUGIN_NATIVEAOT_RUNTIME_ID win-x64 CACHE STRING "Runtime identifier for the C# NativeAOT smoke publish")
    add_custom_target(CSharpTestPluginNativeAotPublish
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CSHARP_TEST_PLUGIN_BINARY_ROOT}/nativeaot/${CSHARP_TEST_PLUGIN_CONFIGURATION}"
        COMMAND "${DotNet_EXECUTABLE}" publish "${CSHARP_TEST_PLUGIN_PROJECT}"
            --framework net8.0
            --configuration "${CSHARP_TEST_PLUGIN_CONFIGURATION}"
            --runtime "${CSHARP_TEST_PLUGIN_NATIVEAOT_RUNTIME_ID}"
            --self-contained true
            --output "${CSHARP_TEST_PLUGIN_BINARY_ROOT}/nativeaot/${CSHARP_TEST_PLUGIN_CONFIGURATION}"
            -p:PublishAot=true
            -p:DasPluginAssemblyName="${CSHARP_TEST_PLUGIN_MODERN_NAME}"
            -p:DasGeneratedProject="${DAS_CORE_CSHARP_PROJECT_FILE}"
            -p:DasGeneratedOutputDir="${DAS_CORE_CSHARP_OUTPUT_DIR}"
            -p:TargetFrameworks=net8.0
            -v:minimal
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "${CMAKE_CURRENT_SOURCE_DIR}/bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/obj"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM)
    add_dependencies(CSharpTestPluginNativeAotPublish DasCoreCSharpExport)
endif()
