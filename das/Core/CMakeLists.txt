add_library(DasCoreObjects OBJECT)
add_custom_target(DasCoreObjectsPreBuild ALL)
add_dependencies(DasCoreObjects DasCoreObjectsPreBuild)
add_dependencies(DasCoreObjectsPreBuild DasAutoCopyDll)

message("Python3 is required because we need to run script.")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# 枚举所有头文件
file(GLOB_RECURSE DAS_EXPORT_HEADERS "${CMAKE_SOURCE_DIR}/include/das/*")
set(DAS_AUTOGEN_OUTPUT_FILES
    "${CMAKE_BINARY_DIR}/DAS/include/DAS/_autogen/OfficialIids.h"
    "${CMAKE_BINARY_DIR}/DAS/include/DAS/_autogen/OfficialIids.cpp"
    "${CMAKE_BINARY_DIR}/DAS/include/DAS/_autogen/CppSwigBiMap.h"
    "${CMAKE_BINARY_DIR}/DAS/include/DAS/_autogen/CppSwigBiMap.cpp"
    # "${CMAKE_BINARY_DIR}/DAS/include/DAS/_autogen/CppSwigInteropFactory.h"
    # "${CMAKE_BINARY_DIR}/DAS/include/DAS/_autogen/CppSwigInteropFactory.cpp"
)
add_custom_command(
    OUTPUT ${DAS_AUTOGEN_OUTPUT_FILES}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/generator.py -I${CMAKE_SOURCE_DIR}/include/das -O${CMAKE_BINARY_DIR}/DAS/include/DAS/_autogen -M0
    DEPENDS ${DAS_EXPORT_HEADERS} ${CMAKE_SOURCE_DIR}/tools/generator.py
    COMMENT "Scan header and generate sources."
)
add_custom_command(
    TARGET DasCoreObjectsPreBuild PRE_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/generator.py -I${CMAKE_SOURCE_DIR}/include/das -O${CMAKE_SOURCE_DIR}/das/Utils/include/das/Utils -M1
    DEPENDS ${DAS_EXPORT_HEADERS} ${CMAKE_SOURCE_DIR}/tools/generator.py
    COMMENT "Generate PresetTypeInheritanceInfo.h."
)

message("Generating source...")
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/generator.py -I${CMAKE_SOURCE_DIR}/include/das -O${CMAKE_BINARY_DIR}/DAS/include/DAS/_autogen -M0
)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/generator.py -I${CMAKE_SOURCE_DIR}/include/das -O${CMAKE_SOURCE_DIR}/das/Utils/include/das/Utils -M1
)

target_compile_options(DasCoreObjects PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)
set_property(TARGET DasCoreObjects PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(DasCoreObjects PUBLIC ${CMAKE_BINARY_DIR}/DAS/include/)
target_sources(DasCoreObjects PRIVATE ${DAS_AUTOGEN_OUTPUT_FILES})
target_compile_definitions(DasCoreObjects PRIVATE -DDAS_BUILD_SHARED)
target_link_libraries(DasCoreObjects PUBLIC
    Boost::filesystem
    ICU::data
    ICU::i18n
    ICU::uc)

if(DAS_USE_BUNDLED_BOOST)
    target_link_libraries(DasCoreObjects PUBLIC ${DAS_BUNDLED_BOOST_LIBS})
else()
    # 对于使用msys包管理器安装boost的用户，使用msys的clangd或使用这个语句强制将Boost_INCLUDE_DIRS添加到compile_commands.json
    # target_compile_options(DasCoreObjects PRIVATE "-I${Boost_INCLUDE_DIRS}")
    target_include_directories(DasCoreObjects PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(DasCoreObjects PRIVATE ${Boost_LIBRARIES})
endif()

if(WIN32)
    target_compile_definitions(DasCoreObjects PRIVATE DAS_PLATFORM="WINDOWS" DAS_WINDOWS)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    target_compile_definitions(DasCoreObjects PRIVATE DAS_PLATFORM="MACOS")
elseif(UNIX)
    target_compile_definitions(DasCoreObjects PRIVATE DAS_PLATFORM="LINUX")
else()
    message(FATAL_ERROR "Unsupported platform.")
endif()

das_add_core_component(Utils)
das_add_core_component(OcvWrapper)
das_add_core_component(OrtWrapper)
das_add_core_component(ForeignInterfaceHost)
das_add_core_component(SettingsManager)
das_add_core_component(Logger)
das_add_core_component(Exceptions)
das_add_core_component(i18n)
das_add_core_component(TaskScheduler)

add_library(DasCore SHARED)
add_dependencies(DasCore DasCoreObjects)

add_compile_definitions(DAS_BUILD_SHARED)

# 配置导出库
set(DAS_EXPORT_MACRO_LIST "")
set(DAS_FOREIGN_LANGUAGE_LIB_LIST "")

cmake_policy(SET CMP0078 NEW)

file(GLOB DAS_SWIG_FILES ${CMAKE_SOURCE_DIR}/SWIG/*.i)
set_property(SOURCE ${DAS_SWIG_FILES} PROPERTY CPLUSPLUS ON)
set_source_files_properties(${DAS_SWIG_FILES} PROPERTIES USE_SWIG_DEPENDENCIES TRUE)

if(${EXPORT_CSHARP})
    das_add_swig_export_library(CSharp DasCoreCSharpExport "${DAS_SWIG_FILES}")
    target_link_libraries(DasCoreCSharpExport PUBLIC DasCore)
    add_dependencies(DasCoreCSharpExport DasCore)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_CSHARP)
endif()

if(${EXPORT_JAVA})
    find_package(JNI REQUIRED)
    das_add_swig_export_library(Java DasCoreJavaExport "${DAS_SWIG_FILES}")
    set_property(TARGET DasCoreJavaExport PROPERTY SWIG_COMPILE_OPTIONS -package org.das)

    target_link_libraries(DasCoreObjects PUBLIC JNI::JNI)

    target_link_libraries(DasCoreJavaExport PUBLIC DasCore)
    add_dependencies(DasCoreJavaExport DasCore)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_JAVA)
endif()

if(${EXPORT_PYTHON})
    find_package(Python3 COMPONENTS Development REQUIRED)
    das_add_swig_export_library(Python DasCorePythonExport "${DAS_SWIG_FILES}")
    set_property(TARGET DasCorePythonExport PROPERTY SWIG_COMPILE_OPTIONS -threads)

    target_include_directories(DasCoreObjects PUBLIC ${Python3_INCLUDE_DIRS})
    target_link_libraries(DasCoreObjects PUBLIC ${Python3_LIBRARIES})

    target_link_libraries(DasCorePythonExport PUBLIC DasCore)
    add_dependencies(DasCorePythonExport DasCore)
    set_target_properties(
        DasCorePythonExport
        PROPERTIES
        DEBUG_POSTFIX "_d"
    )
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_PYTHON)
endif()

# FetchContent_Declare(
# Sol2
# URL ${GITHUB_MIRROR_URL}/ThePhD/sol2/archive/refs/tags/v3.3.0.zip
# )
# FetchContent_MakeAvailable(Sol2)
# target_link_libraries(DasCoreObjects PUBLIC sol2::sol2)
# find_package(Lua 5.4)

# if(NOT ${LUA_FOUND})
# message(STATUS "Downloading and configuring Lua 5.4.6.")
# FetchContent_Declare(
# DasLua
# URL ${GITHUB_MIRROR_URL}/lua/lua/archive/refs/tags/v5.4.6.zip
# PATCH_COMMAND ${CMAKE_COMMAND} -E copy
# "${CMAKE_SOURCE_DIR}/cmake/CompileLua.cmake" <SOURCE_DIR>/CMakeLists.txt
# )
# FetchContent_MakeAvailable(DasLua)
# target_link_libraries(DasCoreObjects PUBLIC Das::Lua)
# else()
# target_include_directories(DasCoreObjects PRIVATE ${LUA_INCLUDE_DIR})
# target_link_libraries(DasCoreObjects PUBLIC ${LUA_LIBRARIES})
# endif()

# list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_LUA)
target_link_libraries(DasCoreObjects
    PUBLIC DasUtils ${DAS_FOREIGN_LANGUAGE_LIB_LIST})
target_compile_definitions(DasCoreObjects PUBLIC ${DAS_EXPORT_MACRO_LIST})

target_link_libraries(DasCore PUBLIC DasCoreObjects)

if(${DAS_BUILD_TEST})
    das_add_core_test(ForeignInterfaceHost)
    das_add_core_test(i18n)
endif()
