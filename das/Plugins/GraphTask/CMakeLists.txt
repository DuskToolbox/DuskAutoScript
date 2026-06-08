aux_source_directory(src DAS_GRAPHTASK_SOURCES)

    add_library(GraphTask SHARED ${DAS_GRAPHTASK_SOURCES}) target_include_directories(
        GraphTask PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} / include)
        target_compile_options(
            GraphTask PRIVATE $<$<CXX_COMPILER_ID : MSVC> : / W4 / WX / MP> $<
                $<NOT : $<CXX_COMPILER_ID : MSVC>> : -Wall - Wextra - Wpedantic
                - Werror>) target_link_libraries(GraphTask PUBLIC DAS_EX_PRIVATE_LIBS)
            target_link_libraries(GraphTask PRIVATE DasCore)

                configure_file(
                    ${CMAKE_CURRENT_SOURCE_DIR}
                    / GraphTask.json ${CMAKE_BINARY_DIR} / das / tmp
                    / GraphTask.json @ONLY)

                    add_custom_command(
                        TARGET GraphTask POST_BUILD COMMAND ${CMAKE_COMMAND}
                            - E copy_if_different ${CMAKE_BINARY_DIR} / das
                                  / tmp / GraphTask.json $
                        < TARGET_FILE_DIR : GraphTask > / GraphTask.json)

                        if (DAS_BUILD_TEST) set(
                            DAS_GRAPHTASK_TEST_PLUGIN_SOURCES ${
                                DAS_GRAPHTASK_SOURCES})
                            list(
                                FILTER DAS_GRAPHTASK_TEST_PLUGIN_SOURCES
                                    EXCLUDE REGEX "DllMain\\.cpp$")

                                aux_source_directory(
                                    test DAS_GRAPHTASK_TEST_SOURCES)

                                    add_executable(
                                        GraphTaskTest ${
                                            DAS_GRAPHTASK_TEST_PLUGIN_SOURCES} ${
                                            DAS_GRAPHTASK_TEST_SOURCES})
                                        target_link_libraries(
                                            GraphTaskTest PRIVATE Das3rdParty
                                                GTest::gtest_main GTest::gtest
                                                    DasCoreObjects)

                                            get_target_property(
                                                DAS_CORE_INCLUDE_DIRS
                                                    DasCoreObjects
                                                        INCLUDE_DIRECTORIES)
                                                target_include_directories(
                                                    GraphTaskTest PRIVATE ${
                                                        DAS_CORE_INCLUDE_DIRS} ${
                                                        CMAKE_CURRENT_SOURCE_DIR}
                                                    / include)

                                                    set_target_properties(
                                                        GraphTaskTest PROPERTIES
                                                            RUNTIME_OUTPUT_DIRECTORY
                                                                ${CMAKE_BINARY_DIR}
                                                        / Test)

                                                        add_dependencies(
                                                            GraphTaskTest GraphTask
                                                                DasAutoCopyDll
                                                                    DasAutoCopyPluginMetadataFile)

                                                            das_get_test_runtime_environment(
                                                                das_graphtask_test_env)
                                                                gtest_discover_tests(
                                                                    GraphTaskTest
                                                                        WORKING_DIRECTORY
                                                                            ${CMAKE_BINARY_DIR}
                                                                    / Test
                                                                        PROPERTIES ENVIRONMENT
                                                                    "${das_graphtask_test_env}")
                                                                    endif()
