aux_source_directory(src DAS_GATEWAY_SOURCES)
file(GLOB_RECURSE include/* DAS_GATEWAY_HEADERS)
add_library(DasGateway SHARED ${DAS_GATEWAY_SOURCES} ${DAS_GATEWAY_HEADERS})
# set_property(TARGET DasGateway PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(DasGateway PUBLIC include)
target_include_directories(DasGateway PRIVATE ../Core/Exceptions/include)
# 添加abi目录包含路径，以便访问生成的IDL ABI文件（如IDasTypeInfo.h）
target_include_directories(DasGateway PRIVATE ${CMAKE_BINARY_DIR}/das/include/DAS/_autogen/idl/abi)
target_link_libraries(DasGateway PUBLIC Das3rdParty DasUtils DasCommonHeaders Boost::filesystem)
if(DAS_USE_BUNDLED_BOOST)
    target_link_libraries(DasGateway PUBLIC Boost::dll)
else()
    target_include_directories(DasGateway PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(DasGateway PRIVATE ${Boost_LIBRARIES})
endif()
if(WIN32)
    target_compile_definitions(DasGateway PRIVATE DAS_WINDOWS)
endif()
target_compile_definitions(DasGateway PRIVATE
    DAS_GATEWAY_BUILD_SHARED
    DAS_CORE_DLL="$<TARGET_FILE_NAME:DasCore>"
    DAS_GATEWAY_NAME="$<TARGET_FILE_NAME:DasGateway>")