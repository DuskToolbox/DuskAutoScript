aux_source_directory(src DAS_HOST_SOURCES)
file(GLOB_RECURSE include/* DAS_HOST_HEADERS)

add_executable(DasHostX ${DAS_HOST_SOURCES} ${DAS_HOST_HEADERS})

target_include_directories(DasHostX PUBLIC include)
target_include_directories(DasHostX PRIVATE ../Core/IPC/include)
target_include_directories(DasHostX PRIVATE ../Core/Utils/include)
target_include_directories(DasHostX PRIVATE ../Utils/include)
target_include_directories(DasHostX PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(DasHostX PRIVATE ${CMAKE_BINARY_DIR}/das/include)

target_link_libraries(DasHostX PRIVATE DasCore DasCommonHeaders DasPlatformDefs)

# 使用 DasCore 的导出宏定义
if(DasCore_EXPORT_DEFINITIONS)
    target_compile_definitions(DasHostX PUBLIC ${DasCore_EXPORT_DEFINITIONS})
endif()
