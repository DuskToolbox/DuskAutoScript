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
das_add_plugin_library(DasWindowsCapture DAS_EX_PRIVATE_LIBS)

if(DAS_BUILD_TEST)
    das_add_plugin_library(IpcTestPlugin DAS_EX_PRIVATE_LIBS)
    
    # Java 测试插件（仅在启用 Java 导出时构建）
    if(EXPORT_JAVA)
        add_subdirectory(JavaTestPlugin)
    endif()
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
