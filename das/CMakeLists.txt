set(CMAKE_FOLDER "AutoStarRail")

add_compile_definitions($<$<CONFIG:Debug>:DEBUG>)

aux_source_directory(Utils/src DAS_UTILS_SOURCES)
file(GLOB_RECURSE Utils/include/* DAS_UTILS_HEADERS)
add_library(DasUtils STATIC ${DAS_UTILS_SOURCES} ${DAS_UTILS_HEADERS})
set_property(TARGET DasUtils PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(DasUtils PUBLIC Utils/include)
target_link_libraries(DasUtils PUBLIC Das3rdParty)

# Prevent visual studio treat headers as sources.
#file(GLOB_RECURSE DAS_COMMON_HEADERS ${CMAKE_SOURCE_DIR}/include/*) ${DAS_COMMON_HEADERS}
add_library(DasCommonHeaders INTERFACE)
target_include_directories(DasCommonHeaders INTERFACE ${CMAKE_SOURCE_DIR}/include/)
target_link_libraries(DasUtils PUBLIC DasCommonHeaders)

add_library(DAS_EX_PRIVATE_LIBS ALIAS DasUtils)

if(${DAS_BUILD_TEST})
    include(GoogleTest)
    das_add_additional_test(AdditionalTest)
    target_link_libraries(AdditionalTest PUBLIC DasUtils Boost::url)
endif()

# 未来Gateway将以独立进程的形式来转发请求到具体的DasCore实例上，但是目前，Gateway即为Core
add_subdirectory(Gateway)

add_subdirectory(Core)

add_subdirectory(Plugins)

add_subdirectory(Http)
