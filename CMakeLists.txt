cmake_minimum_required(VERSION 3.24)

project(AutoStarRail LANGUAGES CXX VERSION 0.1)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if (POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

if(MSVC)
    if(MSVC_VERSION LESS 1939)
        message(FATAL_ERROR "MSVC STL has bug in this_thread::sleep_until and this_thread::sleep_for.\n"
            "See https://github.com/microsoft/STL/pull/3914 https://github.com/microsoft/STL/wiki/Changelog#expected-in-vs-2022-179-preview-2")
    endif()
endif()

if(WIN32)
    set(ASR_LIB_PREFIX "Asr")
else()
    set(ASR_LIB_PREFIX "libAsr")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# fix missing CMAKE_ASM_MASM_COMPILE_OBJECT or CMAKE_ASM_NASM_COMPILE_OBJECT
# NOTICE: I have modified the CMakeLists.txt in boost context, so that boost can explicitly use MASM on Windows.
#         Then I MUST enable ASM_NASM after ASM_MASM, therefore CMake will compile asm files in OpenCV using NASM compiler as default asm compiler.
if (CMAKE_ASM_MASM_COMPILER)
    enable_language(ASM_MASM)
endif ()
if (CMAKE_ASM_NASM_COMPILER)
    enable_language(ASM_NASM)
endif ()

option(EXPORT_JAVA "导出Java接口" OFF)
option(EXPORT_CSHARP "导出C#接口" OFF)
option(EXPORT_PYTHON "导出Python接口" OFF)

# See [C++ 20 Compiler support](https://en.cppreference.com/w/cpp/compiler_support/20)
include(AsrCheckStdFmt)

set(ASR_SPD_FMT_OPTION_EXPLANATION "项目自带的spdlog使用标准库的fmt，OFF则会使用项目附带的fmt")
if (ASR_STD_FMT_SUPPORT)
    option(ASR_SPDLOG_USE_STD_FMT ${ASR_SPD_FMT_OPTION_EXPLANATION} ON)
    option(ASR_USE_BUNDLED_FMT "" OFF)
else ()
    option(ASR_SPDLOG_USE_STD_FMT ${ASR_SPD_FMT_OPTION_EXPLANATION} OFF)
    option(ASR_USE_BUNDLED_FMT "" ON)
endif ()

if (ASR_USE_BUNDLED_FMT)
    set(ASR_SPDLOG_USE_STD_FMT OFF)
endif ()

option(ASR_BUILD_TEST "构建测试，会下载GTEST库" OFF)
option(ASR_BUILD_QT5_GUI "构建基于Qt5的GUI" ON)
option(ASR_USE_LLD "Using lld instead of using ld." OFF)
option(ASR_USE_CCACHE "Using CCACHE" OFF)

option(ASR_USE_BUNDLED_EIGEN "强制使用下载的Eigen源码构建OpenCV" OFF)
option(ASR_USE_BUNDLED_BOOST "强制使用下载的Boost源码构建" OFF)
option(ASR_USE_BUNDLED_OPENCV "强制使用下载的OpenCV源码构建" OFF)

option(ASR_USE_LLD "使用lld进行链接（实验性）" OFF)
option(ASR_USE_MOLD "使用mold进行链接（实验性）" OFF)

if (NOT DEFINED GITHUB_MIRROR_URL)
    set(GITHUB_MIRROR_URL "https://github.com")
endif ()

# TODO: 配置时打印当前设置
# get_property(result CACHE SampleOpt PROPERTY HELPSTRING)

set(ASR_DEFAULT_SHARED_LIBRARY_PREFIX ${CMAKE_SHARED_LIBRARY_PREFIX})
set(ASR_DEFAULT_STATIC_LIBRARY_PREFIX ${CMAKE_STATIC_LIBRARY_PREFIX})
set(ASR_DEFAULT_SHARED_LIBRARY_PREFIX_CXX ${CMAKE_SHARED_LIBRARY_PREFIX_CXX})
set(ASR_DEFAULT_STATIC_LIBRARY_PREFIX_CXX ${CMAKE_STATIC_LIBRARY_PREFIX_CXX})
set(ASR_DEFAULT_SHARED_LIBRARY_PREFIX_C ${CMAKE_SHARED_LIBRARY_PREFIX_C})
set(ASR_DEFAULT_STATIC_LIBRARY_PREFIX_C ${CMAKE_STATIC_LIBRARY_PREFIX_C})

include(AsrUtils)

include(AsrSetOutputPath)

if (${EXPORT_PYTHON})
    set(Python3_FIND_STRATEGY LOCATION)
    find_package(Python3 COMPONENTS Development REQUIRED)
endif ()

include(3rdParty/CMakeLists.txt)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (${ASR_USE_CCACHE})
    find_program(CCACHE "ccache")

    if (CCACHE)
        set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
    endif ()
endif ()

if (WIN32)
    add_definitions(-DNOMINMAX -D_USE_MATH_DEFINES)
endif ()

if (MSVC)
    add_compile_options(/Zc:__cplusplus /utf-8)
endif ()

if (${ASR_USE_LLD} AND NOT MSVC)
    option(ASR_LLD_EXECUTABLE "lld")
    find_program(
            LLD_FOUND
            lld REQUIRED)
    add_compile_options("-fuse-ld=lld")
endif ()

if (${ASR_USE_MOLD} AND NOT MSVC)
    option(ASR_MOLD_EXECUTABLE "mold")
    find_program(
            MOLD_FOUND
            mold REQUIRED)
    add_compile_options("-fuse-ld=mold")
endif ()

configure_file(${CMAKE_SOURCE_DIR}/cmake/CTestCustom.cmake ${CMAKE_BINARY_DIR} @ONLY)

add_custom_target(RemoveSwigOutput
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/AsrRemoveSwigOutput.cmake)

if (${ASR_BUILD_TEST})
    enable_testing()
endif ()

add_subdirectory(AutoStarRail)