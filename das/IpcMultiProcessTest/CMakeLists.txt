# IpcMultiProcessTest - 多进程测试组件

# 如果启用了测试构建，则添加 IPC 多进程测试
if (${DAS_BUILD_TEST})
    # 添加 IPC 多进程测试
    aux_source_directory(test IpcMultiProcessTest_SOURCES)
    set(IpcMultiProcessTest_NAME "IpcMultiProcessTest")
    add_executable(${IpcMultiProcessTest_NAME} ${IpcMultiProcessTest_SOURCES})
    
    # 链接必要的库
    target_link_libraries(${IpcMultiProcessTest_NAME} 
        PUBLIC 
        Das3rdParty 
        GTest::gtest_main 
        GTest::gtest 
        DasCoreObjects
    )

    # 添加包含目录 - IPC 核心头文件
    target_include_directories(${IpcMultiProcessTest_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/das/Core/IPC/include
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/das/include
    )

    # 强制 cmake 输出测试可执行文件到 ${CMAKE_BINARY_DIR}/Test
    set_target_properties(${IpcMultiProcessTest_NAME} PROPERTIES 
        RUNTIME_DIRECTORY ${CMAKE_BINARY_DIR}/Test
        RUNTIME_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Test
        RUNTIME_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Test
        RUNTIME_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/Test
        RUNTIME_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/Test
    )

    # 使用 gtest_discover_tests 注册测试
    gtest_discover_tests(
        ${IpcMultiProcessTest_NAME}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Test
    )

    # 将测试添加到构建锁依赖中
    add_dependencies(${IpcMultiProcessTest_NAME} DasAutoCopyDll)
endif()