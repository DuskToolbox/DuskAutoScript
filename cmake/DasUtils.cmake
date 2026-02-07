function(das_add_library TYPE SUB_DIRECTORY_NAME PRIVATE_EX_LIBS)
    # file(GLOB_RECURSE HEADERS
    # ${CMAKE_CURRENT_SOURCE_DIR}/${SUB_DIRECTORY_NAME}/include/*)
    file(GLOB SOURCES ${SUB_DIRECTORY_NAME}/src/*)
    add_library(${SUB_DIRECTORY_NAME} ${TYPE} ${SOURCES})
    target_include_directories(${SUB_DIRECTORY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/${SUB_DIRECTORY_NAME}/include)
    target_compile_options(${SUB_DIRECTORY_NAME} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
    )

    if(${TYPE} STREQUAL "SHARED")
        target_compile_definitions(${SUB_DIRECTORY_NAME} PRIVATE -DDAS_BUILD_SHARED)
    endif()

    target_link_libraries(${SUB_DIRECTORY_NAME} PUBLIC ${PRIVATE_EX_LIBS})
endfunction()

function(das_add_plugin_library SUB_DIRECTORY_NAME PRIVATE_EX_LIBS)
    # file(GLOB_RECURSE HEADERS
    # ${CMAKE_CURRENT_SOURCE_DIR}/${SUB_DIRECTORY_NAME}/include/*)
    file(GLOB SOURCES ${SUB_DIRECTORY_NAME}/src/*)
    add_library(${SUB_DIRECTORY_NAME} SHARED ${SOURCES})
    target_include_directories(${SUB_DIRECTORY_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/${SUB_DIRECTORY_NAME}/include)
    target_compile_options(${SUB_DIRECTORY_NAME} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
    )
    target_link_libraries(${SUB_DIRECTORY_NAME} PUBLIC ${PRIVATE_EX_LIBS})
    target_link_libraries(${SUB_DIRECTORY_NAME} PRIVATE DasCore)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/${SUB_DIRECTORY_NAME}/${SUB_DIRECTORY_NAME}.json
        ${CMAKE_BINARY_DIR}/das/tmp/${SUB_DIRECTORY_NAME}.json
        @ONLY)
    add_custom_command(
        TARGET DasAutoCopyPluginMetadataFile
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/das/tmp/${SUB_DIRECTORY_NAME}.json $<TARGET_FILE_DIR:${SUB_DIRECTORY_NAME}>)
endfunction()

function(das_add_core_component SUB_DIRECTORY_NAME)
    file(GLOB_RECURSE HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/${SUB_DIRECTORY_NAME}/include/*)
    file(GLOB_RECURSE SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${SUB_DIRECTORY_NAME}/src/*)
    SET(TMP_TARGET_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${SUB_DIRECTORY_NAME}/include/)
    if(EXISTS ${TMP_TARGET_INCLUDE_DIR})
        target_include_directories(DasCoreObjects PUBLIC ${TMP_TARGET_INCLUDE_DIR})
    endif()
    target_sources(DasCoreObjects PRIVATE ${SOURCES} ${HEADERS})
    source_group("${SUB_DIRECTORY_NAME}\\Headers" FILES ${HEADERS})
    source_group("${SUB_DIRECTORY_NAME}\\Sources" FILES ${SOURCES})
endfunction()

macro(das_force_set VARIABLE_NAME VARIABLE_VALUE VARIABLE_TYPE DOC_VALUE)
    set(${VARIABLE_NAME} ${VARIABLE_VALUE} CACHE ${VARIABLE_TYPE} ${DOC_VALUE} FORCE)
endmacro()

function(das_add_additional_test ADDITIONAL_TEST_DIRECTORY_NAME)
    aux_source_directory(${ADDITIONAL_TEST_DIRECTORY_NAME} SOURCES)
    add_executable(AdditionalTest ${SOURCES})
    target_link_libraries(AdditionalTest PRIVATE GTest::gtest_main GTest::gtest)
    add_dependencies(AdditionalTest DasAutoCopyDll)

    # force cmake output test executable to ${CMAKE_BINARY_DIR}/Test
    set_target_properties(AdditionalTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Test)
    set_target_properties(AdditionalTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Test)
    set_target_properties(AdditionalTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Test)
    set_target_properties(AdditionalTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/Test)
    set_target_properties(AdditionalTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/Test)

    gtest_discover_tests(
        AdditionalTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Test)
endfunction()

function(das_add_core_test TEST_FOLDER)
    aux_source_directory(${TEST_FOLDER}/test TEST_SOURCES)
    set(TEST_NAME "${TEST_FOLDER}Test")
    add_executable(${TEST_NAME} ${TEST_SOURCES})
    target_link_libraries(${TEST_NAME} PUBLIC Das3rdParty GTest::gtest_main GTest::gtest DasCoreObjects)

    get_target_property(INCLUDE_DIRS DasCoreObjects INCLUDE_DIRECTORIES)
    target_include_directories(${TEST_NAME} PRIVATE ${INCLUDE_DIRS})

    # force cmake output test executable to ${CMAKE_BINARY_DIR}/Test
    set_target_properties(${TEST_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Test)
    set_target_properties(${TEST_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Test)
    set_target_properties(${TEST_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Test)
    set_target_properties(${TEST_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/Test)
    set_target_properties(${TEST_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/Test)

    gtest_discover_tests(
        ${TEST_NAME}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Test)
endfunction()

function(das_add_swig_export_library LANGUAGE RAW_NAME FILES)
    # 计算SWIG输出目录
    set(SWIG_OUTPUT_DIR ${CMAKE_BINARY_DIR}/include/das/${LANGUAGE})

    # 设置输出目录到父作用域变量，方便其他地方使用
    set(DAS_SWIG_${LANGUAGE}_OUTPUT_DIR ${SWIG_OUTPUT_DIR} PARENT_SCOPE)

    swig_add_library(
        ${RAW_NAME}
        TYPE SHARED
        LANGUAGE ${LANGUAGE}
        OUTPUT_DIR ${SWIG_OUTPUT_DIR}
        OUTFILE_DIR ${CMAKE_BINARY_DIR}/SwigCpp
        SOURCES ${FILES})
    set_property(
        TARGET ${RAW_NAME}
        PROPERTY SWIG_INCLUDE_DIRECTORIES
            ${CMAKE_SOURCE_DIR}/include/
            ${CMAKE_BINARY_DIR}/das/include
            ${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/abi
            ${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/swig)
    add_library(${PROJECT_NAME}::${LANGUAGE}Export ALIAS ${RAW_NAME})
    target_compile_definitions(${RAW_NAME} PRIVATE -DSWIG_TYPE_TABLE=${RAW_NAME})

    # 添加SWIG清理命令，清理未使用的SWIGTYPE_p类型文件（仅Java和C#）
    # 创建独立的清理命令，依赖于SWIG输出文件，确保每次SWIG编译后都执行清理
    if(LANGUAGE STREQUAL "Java" OR LANGUAGE STREQUAL "CSharp")
        # 转换为小写用于参数名
        string(TOLOWER ${LANGUAGE} LANGUAGE_LOWER)
        
        # 清理报告文件路径
        set(CLEANUP_REPORT_FILE ${CMAKE_BINARY_DIR}/das/swig_cleanup_${LANGUAGE_LOWER}_${RAW_NAME}.json)
        
        # 获取SWIG生成的文件列表
        get_target_property(SWIG_OUTPUT_FILES ${RAW_NAME} SWIG_OUTPUT_FILES)
        
        # 创建清理命令，依赖所有SWIG输出文件
        add_custom_command(
            OUTPUT ${CLEANUP_REPORT_FILE}
            COMMAND ${DAS_IDL_VENV_PYTHON}
                ${CMAKE_SOURCE_DIR}/tools/das_idl/clean_swig_unused_types.py
                --${LANGUAGE_LOWER}-dir ${SWIG_OUTPUT_DIR}
                --delete
                --report ${CLEANUP_REPORT_FILE}
            COMMENT "Cleaning unused SWIGTYPE_p files for ${LANGUAGE}"
            DEPENDS ${SWIG_OUTPUT_FILES}
            VERBATIM
        )
        
        # 创建清理目标
        add_custom_target(${RAW_NAME}SwigOutputCleanUp
            DEPENDS ${CLEANUP_REPORT_FILE}
            COMMENT "SWIG cleanup target for ${LANGUAGE}"
        )
        
        # 将清理目标添加到SWIG导出目标的依赖中
        add_dependencies(${RAW_NAME} ${RAW_NAME}SwigOutputCleanUp)
    endif()
endfunction()

function(das_check_language_export LANGUAGE EXPORT_LANGUAGES_LIST)
    if(${EXPORT_${LANGUAGE}})
        string(TOLOWER ${LANGUAGE} LOWERCASE_LANGUAGE)
        list(APPEND ${EXPORT_LANGUAGES_LIST} ${LOWERCASE_LANGUAGE})
        set(${EXPORT_LANGUAGES_LIST} ${${EXPORT_LANGUAGES_LIST}} PARENT_SCOPE)
    endif()
endfunction()

macro(das_add_auto_copy_dll_path DLL_PATH)
    add_custom_command(
        TARGET DasAutoCopyDll
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLL_PATH} $<TARGET_FILE_DIR:DasCore>)

    if(DAS_BUILD_TEST)
        add_custom_command(
            TARGET DasAutoCopyDll
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLL_PATH} ${CMAKE_BINARY_DIR}/Test)
    endif()
endmacro()