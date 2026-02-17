# 根据导出语言查找依赖包
if(EXPORT_JAVA)
    find_package(JNI REQUIRED)
endif()
if(EXPORT_PYTHON)
    find_package(Python3 COMPONENTS Development REQUIRED)
endif()

add_library(DasCoreObjects OBJECT)
add_custom_target(DasCoreObjectsPreBuild ALL)
add_dependencies(DasCoreObjects DasCoreObjectsPreBuild)
add_dependencies(DasGateway DasCoreObjectsPreBuild)

# ===== das_idl模块工具集（未来独立模块）=====
# 使用GLOB自动收集das_idl目录下的所有Python脚本
file(GLOB DAS_IDL_MODULE_TOOLS
    "${CMAKE_SOURCE_DIR}/tools/das_idl/*.py"
)

message(STATUS "[DAS IDL] Module tools (${CMAKE_COUNT_VAR} files):")
foreach(tool ${DAS_IDL_MODULE_TOOLS})
    message(STATUS "  - ${tool}")
endforeach()

# 引入 DasIdlGenerator.cmake 模块
if (NOT EXISTS "${CMAKE_SOURCE_DIR}/cmake/DasIdlGenerator.cmake")
    message(FATAL_ERROR "DasIdlGenerator.cmake not found at ${CMAKE_SOURCE_DIR}/cmake/DasIdlGenerator.cmake")
endif ()
include("${CMAKE_SOURCE_DIR}/cmake/DasIdlGenerator.cmake")
message(STATUS "[DAS IDL] DasIdlGenerator.cmake module loaded successfully")
include("${CMAKE_SOURCE_DIR}/cmake/DasAddIdlExport.cmake")

# 确保Python虚拟环境已创建（在CMake配置阶段运行）
message(STATUS "[DAS IDL] Setting up Python virtual environment...")
das_idl_setup_venv()
if (NOT EXISTS "${DAS_IDL_VENV_PYTHON}")
    message(FATAL_ERROR "[DAS IDL] Python virtual environment setup failed. ${DAS_IDL_VENV_PYTHON} does not exist.")
endif ()
message(STATUS "[DAS IDL] Python virtual environment is ready at ${DAS_IDL_VENV_PYTHON}")

# 收集所有IDL文件
file(GLOB DAS_IDL_ROOT "${CMAKE_SOURCE_DIR}/idl/*.idl")

# ===== 测试用 IDL 文件 =====
# 测试 IDL 文件有独立的目录结构: das/*/test/idl/*.idl
# 这些 IDL 文件仅用于测试目的，不参与 SWIG 导出
file(GLOB_RECURSE DAS_IDL_TEST
    "${CMAKE_SOURCE_DIR}/das/*/test/idl/*.idl"
)

set(DAS_IDL_FILES ${DAS_IDL_ROOT})

# 使用 das_add_idl_export 函数替换现有的 IDL 生成和 SWIG 导出代码
# 动态构建 LANGUAGES 参数
set(_LANGUAGES "")
if(EXPORT_PYTHON)
    list(APPEND _LANGUAGES "Python")
endif()
if(EXPORT_JAVA)
    list(APPEND _LANGUAGES "Java")
endif()
if(EXPORT_CSHARP)
    list(APPEND _LANGUAGES "CSharp")
endif()

# 获取 idl/ 目录下所有 .idl 文件名
file(GLOB _IDL_FILES "${CMAKE_SOURCE_DIR}/idl/*.idl")
set(_IDL_FILE_NAMES "")
foreach(_FILE ${_IDL_FILES})
    get_filename_component(_NAME ${_FILE} NAME)
    list(APPEND _IDL_FILE_NAMES ${_NAME})
endforeach()

# 指定用户自定义的 SWIG 文件（SWIG/ExportAll.i 通过 %include 包含自动生成的内容）
set(_USER_SWIG_FILES "${CMAKE_SOURCE_DIR}/SWIG/ExportAll.i")

# 使用新函数替换现有的 IDL 生成和 SWIG 导出代码
das_add_idl_export(
    NAME DasCore
    IDL_DIR "${CMAKE_SOURCE_DIR}/idl"
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/include/das"
    IDL_FILES ${_IDL_FILE_NAMES}
    LANGUAGES ${_LANGUAGES}
    USER_SWIG_FILES ${_USER_SWIG_FILES}
    SWIG_OPTIONS_Python -threads
    SWIG_OPTIONS_Java -package org.das
    SWIG_INCLUDE_DIRS
        "${CMAKE_SOURCE_DIR}/include/"
        "${CMAKE_BINARY_DIR}/das/include"
    DEPENDS_ON DasCore
)

# 让预构建目标依赖IDL生成目标
# 注意：DasCoreSwigAllGenerated 目标只在启用 SWIG 导出时才存在
add_dependencies(DasCoreObjectsPreBuild DasCoreIdlUpdateList DasCoreIdlGenerated)
if(_LANGUAGES)
    add_dependencies(DasCoreObjectsPreBuild DasCoreSwigAllGenerated)
endif()

# ====== 官方IID列表生成 ======
# 设置OfficialIids输出目录
set(DAS_OFFICIAL_IIDS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/iids")
set(DAS_OFFICIAL_IIDS_HEADER "${DAS_OFFICIAL_IIDS_OUTPUT_DIR}/OfficialIids.h")
set(DAS_OFFICIAL_IIDS_SOURCE "${DAS_OFFICIAL_IIDS_OUTPUT_DIR}/OfficialIids.cpp")
set(DAS_OFFICIAL_IIDS_DEPS_FILE "${CMAKE_BINARY_DIR}/das/official_iids_deps.txt")

# 生成官方IID列表的自定义命令
add_custom_command(
        OUTPUT ${DAS_OFFICIAL_IIDS_HEADER} ${DAS_OFFICIAL_IIDS_SOURCE}
        COMMAND ${CMAKE_COMMAND} -E remove -f ${DAS_OFFICIAL_IIDS_DEPS_FILE}
        COMMAND ${DAS_IDL_VENV_PYTHON} ${CMAKE_SOURCE_DIR}/tools/generate_official_iids.py
        -i ${DAS_IDL_FILES}
        -o ${DAS_OFFICIAL_IIDS_OUTPUT_DIR}
        --check-deps ${DAS_OFFICIAL_IIDS_DEPS_FILE}
        -v
        DEPENDS ${DAS_IDL_FILES}
        ${CMAKE_SOURCE_DIR}/tools/generate_official_iids.py
        ${CMAKE_SOURCE_DIR}/tools/das_idl/das_idl_parser.py
        COMMENT "Generate official IIDs from IDL files"
        VERBATIM
)

# 创建官方IID列表生成目标
add_custom_target(DasCoreOfficialIidsGenerated DEPENDS ${DAS_OFFICIAL_IIDS_HEADER} ${DAS_OFFICIAL_IIDS_SOURCE})
add_dependencies(DasCoreObjectsPreBuild DasCoreOfficialIidsGenerated)
# ====== 官方IID列表生成结束 ======

target_compile_options(DasCoreObjects PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)
set_property(TARGET DasCoreObjects PROPERTY POSITION_INDEPENDENT_CODE ON)

# 初始化外部语言库列表
set(DAS_FOREIGN_LANGUAGE_LIB_LIST "")

# 设置导出宏列表
set(DAS_EXPORT_MACRO_LIST "")
if(EXPORT_PYTHON)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_PYTHON)
    list(APPEND DAS_FOREIGN_LANGUAGE_LIB_LIST Python3::Python)
    target_include_directories(DasCoreObjects PUBLIC ${Python3_INCLUDE_DIRS})
endif()
if(EXPORT_JAVA)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_JAVA)
    list(APPEND DAS_FOREIGN_LANGUAGE_LIB_LIST JNI::JNI)
endif()
if(EXPORT_CSHARP)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_CSHARP)
endif()

# 应用导出宏定义
if(DAS_EXPORT_MACRO_LIST)
    target_compile_definitions(DasCoreObjects PUBLIC ${DAS_EXPORT_MACRO_LIST})
endif()

target_include_directories(DasCoreObjects PUBLIC ${CMAKE_BINARY_DIR}/das/include/)
target_include_directories(DasCoreObjects PUBLIC ${DasCore_ABI_OUTPUT_DIR})
target_include_directories(DasCoreObjects PUBLIC ${DasCore_WRAPPER_OUTPUT_DIR})
target_compile_definitions(DasCoreObjects PRIVATE -DDAS_BUILD_SHARED)
 target_link_libraries(DasCoreObjects PUBLIC
        Boost::filesystem
        ICU::data
        ICU::i18n
        ICU::uc
        Das3rdParty)

if (DAS_USE_BUNDLED_BOOST)
    target_link_libraries(DasCoreObjects PUBLIC ${DAS_BUNDLED_BOOST_LIBS})
else ()
    # 对于使用msys包管理器安装boost的用户，使用msys的clangd或使用这个语句强制将Boost_INCLUDE_DIRS添加到compile_commands.json
    # target_compile_options(DasCoreObjects PRIVATE "-I${Boost_INCLUDE_DIRS}")
    target_include_directories(DasCoreObjects PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(DasCoreObjects PRIVATE ${Boost_LIBRARIES})
endif ()

if (WIN32)
    target_compile_definitions(DasCoreObjects PRIVATE DAS_PLATFORM="WINDOWS" DAS_WINDOWS)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    target_compile_definitions(DasCoreObjects PRIVATE DAS_PLATFORM="MACOS")
elseif (UNIX)
    target_compile_definitions(DasCoreObjects PRIVATE DAS_PLATFORM="LINUX")
else ()
    message(FATAL_ERROR "Unsupported platform.")
endif ()

das_add_core_component(Utils)
das_add_core_component(OcvWrapper)
das_add_core_component(OrtWrapper)
das_add_core_component(ForeignInterfaceHost)

# 将OfficialIids生成的文件添加到DasCoreObjects
set(DAS_OFFICIAL_IIDS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/iids")
# 注意：无条件添加这些文件，因为 add_custom_command 会确保文件在编译前生成
target_sources(DasCoreObjects PRIVATE
        ${DAS_OFFICIAL_IIDS_SOURCE}
        ${DAS_OFFICIAL_IIDS_HEADER}
)
target_include_directories(DasCoreObjects PUBLIC ${DAS_OFFICIAL_IIDS_OUTPUT_DIR})
source_group("Autogen\\OfficialIids" FILES "${DAS_OFFICIAL_IIDS_SOURCE}" "${DAS_OFFICIAL_IIDS_HEADER}")
das_add_core_component(SettingsManager)
das_add_core_component(Logger)
das_add_core_component(Exceptions)
das_add_core_component(i18n)
das_add_core_component(TaskScheduler)
das_add_core_component(IPC)

add_library(DasCore SHARED)
add_dependencies(DasCore DasCoreObjects)

add_compile_definitions(DAS_BUILD_SHARED)

# === DasTest IDL Export ===
# 测试用的 IDL 导出，使用 das_add_idl_export 函数
# 注意：DasTest 不需要 SWIG 导出，只需要 IPC 代码生成
# set(_TEST_IDL_FILES "IDasTestInterface.idl")
#
# das_add_idl_export(
#     NAME DasTest
#     IDL_DIR "${CMAKE_SOURCE_DIR}/das/Core/IPC/test/idl"
#     OUTPUT_DIR "${CMAKE_BINARY_DIR}/das_test/include/das"
#     IDL_FILES ${_TEST_IDL_FILES}
#     LANGUAGES ""  # 测试不需要 SWIG 导出
#     GENERATE_IPC_PROXY ON
#     GENERATE_IPC_STUB ON
#     GENERATE_IPC_MESSAGE ON
# )

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
target_compile_definitions(DasCoreObjects PRIVATE DAS_CORE_NAME="$<TARGET_FILE_NAME:DasCore>")

target_link_libraries(DasCore PUBLIC DasCoreObjects)

 if (${DAS_BUILD_TEST})
     das_add_core_test(ForeignInterfaceHost)
     das_add_core_test(i18n)
     das_add_core_test(Utils)
     das_add_core_test(Exceptions)
     das_add_core_test(IPC)
 endif ()
