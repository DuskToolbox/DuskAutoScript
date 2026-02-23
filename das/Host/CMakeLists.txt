aux_source_directory(src DAS_HOST_SOURCES)
file(GLOB_RECURSE include/* DAS_HOST_HEADERS)

add_executable(DasHost ${DAS_HOST_SOURCES} ${DAS_HOST_HEADERS})

target_include_directories(DasHost PUBLIC include)
target_include_directories(DasHost PRIVATE ../Core/IPC/include)
target_include_directories(DasHost PRIVATE ../Core/Utils/include)
target_include_directories(DasHost PRIVATE ../Utils/include)
target_include_directories(DasHost PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(DasHost PRIVATE ${CMAKE_BINARY_DIR}/das/include)

target_link_libraries(DasHost PRIVATE DasCore DasCommonHeaders)

# 使用 DasCore 的导出宏定义
if(DasCore_EXPORT_DEFINITIONS)
    target_compile_definitions(DasHost PUBLIC ${DasCore_EXPORT_DEFINITIONS})
endif()

if(WIN32)
    target_compile_definitions(DasHost PRIVATE DAS_WINDOWS)
endif()
