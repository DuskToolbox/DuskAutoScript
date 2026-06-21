set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/Debug/plugins)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/lib/Debug/plugins)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/Debug/plugins)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/Release/plugins)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/lib/Release/plugins)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/Release/plugins)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/bin/RelWithDebInfo/plugins)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/lib/RelWithDebInfo/plugins)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/bin/RelWithDebInfo/plugins)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/bin/MinSizeRel/plugins)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/lib/MinSizeRel/plugins)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/bin/MinSizeRel/plugins)

add_custom_target(DasAutoCopyPluginMetadataFile ALL)

das_add_plugin_library(DasAdbCapture DAS_EX_PRIVATE_LIBS)
das_add_plugin_library(DasAdbTouch DAS_EX_PRIVATE_LIBS)
das_add_plugin_library(DasFlowControl DAS_EX_PRIVATE_LIBS)
das_add_plugin_library(DasWindowsCapture DAS_EX_PRIVATE_LIBS)
if(TARGET DasBundledMaaFramework)
    add_subdirectory(DasMaaPi)
endif()

if(TARGET TaskSchedulerTest)
    add_dependencies(TaskSchedulerTest DasFlowControl DasAutoCopyPluginMetadataFile)
endif()

# TaskSchedulerTest 需要先构建 SchedulerTestPlugin（SetUp 部署其 dll+json），
# 并能 include 其共享头 SchedulerTestSharedState.h。
if(TARGET TaskSchedulerTest)
    add_dependencies(TaskSchedulerTest SchedulerTestPlugin)
    target_include_directories(TaskSchedulerTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/SchedulerTestPlugin/include)
    # 暴露 SchedulerTestPlugin 的实际产物文件名给测试源码：
    # MSVC 产出 SchedulerTestPlugin.dll，MinGW/Linux 产出 libSchedulerTestPlugin.{dll,so}。
    # $<TARGET_FILE_NAME:...> 在构建期求值，跨平台得到正确文件名。
    target_compile_definitions(TaskSchedulerTest PRIVATE
        DAS_SCHEDULER_TEST_PLUGIN_FILENAME="$<TARGET_FILE_NAME:SchedulerTestPlugin>")
endif()

if(DAS_BUILD_TEST)
    # Java 测试插件（仅在启用 Java 导出时构建）
    if(EXPORT_JAVA)
        add_subdirectory(JavaTestPlugin)
    endif()

    # Lua 测试插件（仅在启用 Lua 导出时构建）
    if(EXPORT_LUA)
        add_subdirectory(LuaTestPlugin)
    endif()

    # Node 测试插件（仅在启用 Node 导出时构建）
    if(EXPORT_NODE)
        add_subdirectory(NodeTestPlugin)
    endif()

    # C# 测试插件（仅在启用 C# 导出时构建）
    if(EXPORT_CSHARP)
        add_subdirectory(CSharpTestPlugin)
    endif()

    # Python 测试插件（仅在启用 Python 导出时构建）
    if(EXPORT_PYTHON)
        add_subdirectory(PythonTestPlugin)
    endif()
endif()

# IpcTestPlugin1 - IDasTouch Mock for IPC testing
if(DAS_BUILD_TEST)
    das_add_plugin_library(IpcTestPlugin1 DAS_EX_PRIVATE_LIBS)
    # 强制 lib 前缀，匹配 CppPluginLoadTest.IpcTestPlugin1ArtifactsUseLibPrefixedBinaryContract
    # 的"artifacts always use lib prefix"契约。不设此项时 MSVC 产物为 IpcTestPlugin1.dll（无前缀），
    # 违反契约。设置 PREFIX "lib" 后所有平台统一为 libIpcTestPlugin1.{dll|so}。
    set_target_properties(IpcTestPlugin1 PROPERTIES PREFIX "lib")
endif()

# SchedulerTestPlugin - 真实 InProcess Cpp 测试插件，替代死代码 FakeFactoryRuntime。
# TaskSchedulerTest 的 SchedulerRuntimeBackedTest 经生产 CppHost 路径加载本 dll，
# 通过 DasTestPlugin_SetSharedState 注入 exe 持有的 FactoryTaskSharedState 裸指针。
if(DAS_BUILD_TEST)
    das_add_plugin_library(SchedulerTestPlugin DAS_EX_PRIVATE_LIBS)
endif()

# IpcTestPlugin2 - IDasComponent + Factory for IPC testing
if(DAS_BUILD_TEST)
    das_add_plugin_library(IpcTestPlugin2 DAS_EX_PRIVATE_LIBS)
endif()

# DasGraphTask - GraphRuntime thin IDasTask adapter (core-first v18)
if(DAS_BUILD_TEST)
    add_subdirectory(DasGraphTask)
endif()


target_link_libraries(DasAdbCapture PRIVATE zlib)

if(WIN32)
    target_link_libraries(DasAdbCapture PRIVATE Ws2_32 bcrypt)
    target_link_libraries(DasAdbTouch PRIVATE Ws2_32)
    target_link_libraries(DasWindowsCapture PRIVATE Ws2_32)
    
    # WinRT / C++/WinRT headers for DasWindowsCapture (MinGW only, MSVC has built-in SDK support)
    if(MINGW)
        list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
        find_package(WinRT REQUIRED)
        target_include_directories(DasWindowsCapture PRIVATE ${WinRT_INCLUDE_DIR})
        # WinRT headers use __int128 which is a GCC extension, not standard C++
        target_compile_options(DasWindowsCapture PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-Wno-pedantic>
            $<$<CXX_COMPILER_ID:Clang>:-Wno-nonportable-include-path>)
    endif()
    
    # DirectX 11 and Windows Runtime libraries for WindowsGraphicsCapture
    target_link_libraries(DasWindowsCapture PRIVATE
        d3d11.lib
        windowsapp.lib)
endif()



target_include_directories(DasAdbCapture PRIVATE ${gzip-hpp_SOURCE_DIR}/include)

if(DAS_USE_BUNDLED_BOOST)
    target_link_libraries(DasAdbCapture PUBLIC
        Boost::process
        Boost::container_hash
        Boost::url
        Boost::asio
        Boost::pfr)
    target_link_libraries(DasAdbTouch PUBLIC
        Boost::process
        Boost::url
        Boost::asio)
else()
    target_include_directories(DasAdbCapture PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(DasAdbCapture PRIVATE ${Boost_LIBRARIES})

    target_include_directories(DasAdbTouch PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(DasAdbTouch PRIVATE ${Boost_LIBRARIES})
endif()
