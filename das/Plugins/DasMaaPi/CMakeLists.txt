aux_source_directory(src DAS_MAAPI_SOURCES)

add_library(DasMaaPi SHARED ${DAS_MAAPI_SOURCES})
# 强制 lib 前缀，匹配 DasMaaPi 测试套件中 libDasMaaPi.dll 的预期
# （MaapiPluginScaffoldTest.ArtifactsUseOfficialPluginContract、
#  MaapiIntegrationFixture.SampleInterfaceAuthoringCompileRuntimeDryRun 等）。
# MSVC 默认无前缀；设 PREFIX "lib" 后所有平台统一为 libDasMaaPi.{dll|so}。
set_target_properties(DasMaaPi PROPERTIES PREFIX "lib")
target_include_directories(DasMaaPi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(DasMaaPi PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>)
target_link_libraries(DasMaaPi PUBLIC DAS_EX_PRIVATE_LIBS)
target_link_libraries(DasMaaPi PRIVATE
    DasCore
    DasBundledMaaFramework)

if(WIN32)
    target_link_libraries(DasMaaPi PRIVATE bcrypt)
endif()

if(TARGET DasBundledMaaFrameworkDll)
    get_target_property(_das_maapi_framework_implib DasBundledMaaFrameworkDll IMPORTED_IMPLIB)
    get_filename_component(_das_maapi_framework_lib_dir "${_das_maapi_framework_implib}" DIRECTORY)
    set(_das_maapi_agent_client_implib "${_das_maapi_framework_lib_dir}/MaaAgentClient.lib")
    if(NOT EXISTS "${_das_maapi_agent_client_implib}")
        message(FATAL_ERROR "MaaAgentClient.lib not found next to MaaFramework.lib")
    endif()
    target_link_libraries(DasMaaPi PRIVATE "${_das_maapi_agent_client_implib}")
endif()

# DasMaaPi 需要独立子目录布局（与一堆 MaaFramework 第三方 dll 隔离在
# plugins/DasMaaPi/ 下，由 CppHost 用 altered_search_path 从该目录加载依赖）。
# 必须用 per-config 属性覆盖：父目录 das/Plugins/CMakeLists.txt 设了目录级
# CMAKE_RUNTIME_OUTPUT_DIRECTORY_<CONFIG>=plugins，它会初始化 target 的
# per-config 属性，而 per-config 属性优先级高于不带后缀的
# RUNTIME_OUTPUT_DIRECTORY，因此只设不带后缀的属性会被压制、产物落回根目录。
foreach(_das_maapi_cfg "DEBUG;Debug" "RELEASE;Release" "RELWITHDEBINFO;RelWithDebInfo" "MINSIZEREL;MinSizeRel")
    list(GET _das_maapi_cfg 0 _das_maapi_suffix)
    list(GET _das_maapi_cfg 1 _das_maapi_dir)
    set_target_properties(DasMaaPi PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_das_maapi_suffix} "${CMAKE_BINARY_DIR}/bin/${_das_maapi_dir}/plugins/DasMaaPi"
        LIBRARY_OUTPUT_DIRECTORY_${_das_maapi_suffix} "${CMAKE_BINARY_DIR}/bin/${_das_maapi_dir}/plugins/DasMaaPi"
        ARCHIVE_OUTPUT_DIRECTORY_${_das_maapi_suffix} "${CMAKE_BINARY_DIR}/lib/${_das_maapi_dir}/plugins/DasMaaPi")
endforeach()

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
    set(DAS_MAAPI_TEST_PLUGIN_SOURCES ${DAS_MAAPI_SOURCES})
    list(FILTER DAS_MAAPI_TEST_PLUGIN_SOURCES EXCLUDE REGEX "DllMain\.cpp$")

    aux_source_directory(test DAS_MAAPI_TEST_SOURCES)

    add_executable(DasMaaPiTest
        ${DAS_MAAPI_TEST_PLUGIN_SOURCES}
        ${DAS_MAAPI_TEST_SOURCES})
    target_link_libraries(DasMaaPiTest PRIVATE
        Das3rdParty
        GTest::gtest_main
        GTest::gtest
        DasCoreObjects)
    if(WIN32)
        target_link_libraries(DasMaaPiTest PRIVATE bcrypt)
    endif()
    target_compile_definitions(DasMaaPiTest PRIVATE
        DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY)

    get_target_property(DAS_CORE_INCLUDE_DIRS DasCoreObjects INCLUDE_DIRECTORIES)
    target_include_directories(DasMaaPiTest PRIVATE
        ${DAS_CORE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/das/Core/ForeignInterfaceHost/include)

    set_target_properties(DasMaaPiTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Test)

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
