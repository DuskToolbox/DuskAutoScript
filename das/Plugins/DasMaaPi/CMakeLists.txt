file(GLOB DAS_MAAPI_PLUGIN_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*)

add_library(DasMaaPi SHARED ${DAS_MAAPI_PLUGIN_SOURCES})
target_include_directories(DasMaaPi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(DasMaaPi PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>)
target_link_libraries(DasMaaPi PUBLIC DAS_EX_PRIVATE_LIBS)
target_link_libraries(DasMaaPi PRIVATE
    DasCore
    DasBundledMaaFramework)

if(TARGET DasBundledMaaFrameworkDll)
    get_target_property(_das_maapi_framework_implib DasBundledMaaFrameworkDll IMPORTED_IMPLIB)
    get_filename_component(_das_maapi_framework_lib_dir "${_das_maapi_framework_implib}" DIRECTORY)
    set(_das_maapi_agent_client_implib "${_das_maapi_framework_lib_dir}/MaaAgentClient.lib")
    if(NOT EXISTS "${_das_maapi_agent_client_implib}")
        message(FATAL_ERROR "MaaAgentClient.lib not found next to MaaFramework.lib")
    endif()
    target_link_libraries(DasMaaPi PRIVATE "${_das_maapi_agent_client_implib}")
endif()

set(_das_maapi_package_dir "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/plugins/DasMaaPi")
set(_das_maapi_archive_dir "${CMAKE_BINARY_DIR}/lib/$<CONFIG>/plugins/DasMaaPi")
set_target_properties(DasMaaPi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_das_maapi_package_dir}"
    LIBRARY_OUTPUT_DIRECTORY "${_das_maapi_package_dir}"
    ARCHIVE_OUTPUT_DIRECTORY "${_das_maapi_archive_dir}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug/plugins/DasMaaPi"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug/plugins/DasMaaPi"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/lib/Debug/plugins/DasMaaPi"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release/plugins/DasMaaPi"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release/plugins/DasMaaPi"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib/Release/plugins/DasMaaPi"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin/RelWithDebInfo/plugins/DasMaaPi"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin/RelWithDebInfo/plugins/DasMaaPi"
    ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/lib/RelWithDebInfo/plugins/DasMaaPi"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin/MinSizeRel/plugins/DasMaaPi"
    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin/MinSizeRel/plugins/DasMaaPi"
    ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/lib/MinSizeRel/plugins/DasMaaPi")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/DasMaaPi.json
    ${CMAKE_BINARY_DIR}/das/tmp/DasMaaPi.json
    @ONLY)

add_custom_command(
    TARGET DasMaaPi
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_BINARY_DIR}/das/tmp/DasMaaPi.json
        $<TARGET_FILE_DIR:DasMaaPi>/DasMaaPi.json)

if(NOT DAS_MAAFRAMEWORK_RUNTIME_DLLS)
    message(FATAL_ERROR "DAS_MAAFRAMEWORK_RUNTIME_DLLS is empty; cannot assemble DasMaaPi package")
endif()

foreach(_das_maapi_runtime_dll IN LISTS DAS_MAAFRAMEWORK_RUNTIME_DLLS)
    add_custom_command(
        TARGET DasMaaPi
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_das_maapi_runtime_dll}"
            $<TARGET_FILE_DIR:DasMaaPi>)
endforeach()

if(DAS_BUILD_TEST)
    add_executable(DasMaaPiTest
        src/AgentRuntimeRequest.cpp
        src/AuthoringProjector.cpp
        src/MaaRuntime.cpp
        src/MaapiAuthoringSession.cpp
        src/MaapiAuthoringSessionFactory.cpp
        src/MaapiTask.cpp
        src/PiCompiler.cpp
        src/PiCatalog.cpp
        src/PiParser.cpp
        src/PluginImpl.cpp
        src/PluginUtils.cpp
        test/MaapiAuthoringTest.cpp
        test/MaapiCompileTest.cpp
        test/MaapiIntegrationTest.cpp
        test/MaapiAgentRuntimeDtoTest.cpp
        test/MaapiAgentRuntimeLifecycleTest.cpp
        test/MaapiPluginScaffoldTest.cpp
        test/MaapiPiParserTest.cpp
        test/MaapiRuntimeTest.cpp)
    target_link_libraries(DasMaaPiTest PRIVATE
        Das3rdParty
        GTest::gtest_main
        GTest::gtest
        DasCoreObjects)
    target_compile_definitions(DasMaaPiTest PRIVATE
        DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY)

    get_target_property(DAS_CORE_INCLUDE_DIRS DasCoreObjects INCLUDE_DIRECTORIES)
    target_include_directories(DasMaaPiTest PRIVATE
        ${DAS_CORE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/das/Core/ForeignInterfaceHost/include)

    set_target_properties(DasMaaPiTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Test
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Test
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Test
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/Test
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/Test)

    add_dependencies(DasMaaPiTest
        DasMaaPi
        DasAutoCopyDll
        DasAutoCopyPluginMetadataFile)

    add_custom_command(
        TARGET DasMaaPiTest
        PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures
            ${CMAKE_BINARY_DIR}/Test/DasMaaPi/test/fixtures)

    das_get_test_runtime_environment(das_maapi_test_env)
    gtest_discover_tests(
        DasMaaPiTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Test
        PROPERTIES
            ENVIRONMENT "${das_maapi_test_env}")
endif()
