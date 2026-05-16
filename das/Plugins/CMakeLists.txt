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
    das_add_plugin_library(DasMaaPi DAS_EX_PRIVATE_LIBS)
    target_link_libraries(DasMaaPi PRIVATE DasBundledMaaFramework)
endif()

if(TARGET TaskSchedulerTest)
    add_dependencies(TaskSchedulerTest DasFlowControl DasAutoCopyPluginMetadataFile)
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
endif()

if(DAS_BUILD_TEST AND TARGET DasMaaPi)
    add_executable(DasMaaPiTest DasMaaPi/test/MaapiPluginScaffoldTest.cpp)
    target_link_libraries(DasMaaPiTest PRIVATE
        Das3rdParty
        GTest::gtest_main
        GTest::gtest
        DasCoreObjects)

    get_target_property(DAS_CORE_INCLUDE_DIRS DasCoreObjects INCLUDE_DIRECTORIES)
    target_include_directories(DasMaaPiTest PRIVATE
        ${DAS_CORE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/DasMaaPi/include
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

    das_get_test_runtime_environment(das_maapi_test_env)
    gtest_discover_tests(
        DasMaaPiTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Test
        PROPERTIES
            ENVIRONMENT "${das_maapi_test_env}")
endif()

# IpcTestPlugin1 - IDasTouch Mock for IPC testing
if(DAS_BUILD_TEST)
    das_add_plugin_library(IpcTestPlugin1 DAS_EX_PRIVATE_LIBS)
endif()

# IpcTestPlugin2 - IDasComponent + Factory for IPC testing
if(DAS_BUILD_TEST)
    das_add_plugin_library(IpcTestPlugin2 DAS_EX_PRIVATE_LIBS)
endif()


target_link_libraries(DasAdbCapture PRIVATE zlib)

if(WIN32)
    target_link_libraries(DasAdbCapture PRIVATE Ws2_32)
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
