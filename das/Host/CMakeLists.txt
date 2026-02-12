aux_source_directory(src DAS_HOST_SOURCES)
file(GLOB_RECURSE include/* DAS_HOST_HEADERS)

add_executable(DasHost ${DAS_HOST_SOURCES} ${DAS_HOST_HEADERS})

target_include_directories(DasHost PUBLIC include)
target_include_directories(DasHost PRIVATE ../Core/IPC/include)
target_include_directories(DasHost PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(DasHost PRIVATE ${CMAKE_BINARY_DIR}/das/include)

target_link_libraries(DasHost PUBLIC DasCore DasUtils DasCommonHeaders)

if(DAS_USE_BUNDLED_BOOST)
    target_link_libraries(DasHost PUBLIC Boost::dll)
else()
    target_include_directories(DasHost PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(DasHost PRIVATE ${Boost_LIBRARIES})
endif()

if(WIN32)
    target_compile_definitions(DasHost PRIVATE DAS_WINDOWS)
endif()

set_target_properties(DasHost PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Host"
)
