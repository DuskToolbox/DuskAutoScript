aux_source_directory(src DAS_DASGRAPHTASK_SOURCES)

add_library(DasGraphTask SHARED ${DAS_DASGRAPHTASK_SOURCES})
target_include_directories(DasGraphTask PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(DasGraphTask PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>)
target_link_libraries(DasGraphTask PUBLIC DAS_EX_PRIVATE_LIBS)
target_link_libraries(DasGraphTask PRIVATE DasCore)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/DasGraphTask.json
    ${CMAKE_BINARY_DIR}/das/tmp/DasGraphTask.json
    @ONLY)

add_custom_command(
    TARGET DasGraphTask
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_BINARY_DIR}/das/tmp/DasGraphTask.json
        $<TARGET_FILE_DIR:DasGraphTask>/DasGraphTask.json)

if(DAS_BUILD_TEST)
    set(DAS_DASGRAPHTASK_TEST_PLUGIN_SOURCES ${DAS_DASGRAPHTASK_SOURCES})
    list(FILTER DAS_DASGRAPHTASK_TEST_PLUGIN_SOURCES EXCLUDE REGEX "DllMain\\.cpp$")

    aux_source_directory(test DAS_DASGRAPHTASK_TEST_SOURCES)

    add_executable(DasGraphTaskTest
        ${DAS_DASGRAPHTASK_TEST_PLUGIN_SOURCES}
        ${DAS_DASGRAPHTASK_TEST_SOURCES})
    target_link_libraries(DasGraphTaskTest PRIVATE
        Das3rdParty
        GTest::gtest_main
        GTest::gtest
        DasCore)

    target_include_directories(DasGraphTaskTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

    set_target_properties(DasGraphTaskTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Test)

    add_dependencies(DasGraphTaskTest DasGraphTask DasAutoCopyDll DasAutoCopyPluginMetadataFile)

    das_get_test_runtime_environment(das_dasgraphtask_test_env)
    gtest_discover_tests(DasGraphTaskTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Test
        PROPERTIES ENVIRONMENT "${das_dasgraphtask_test_env}")
endif()
