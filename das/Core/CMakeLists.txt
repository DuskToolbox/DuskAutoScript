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

# 确保Python虚拟环境已创建（在CMake配置阶段运行）
message(STATUS "[DAS IDL] Setting up Python virtual environment...")
das_idl_setup_venv()
if (NOT EXISTS "${DAS_IDL_VENV_PYTHON}")
    message(FATAL_ERROR "[DAS IDL] Python virtual environment setup failed. ${DAS_IDL_VENV_PYTHON} does not exist.")
endif ()
message(STATUS "[DAS IDL] Python virtual environment is ready at ${DAS_IDL_VENV_PYTHON}")

# 收集所有IDL文件
file(GLOB DAS_IDL_ROOT "${CMAKE_SOURCE_DIR}/idl/*.idl")
set(DAS_IDL_FILES ${DAS_IDL_ROOT})

# 设置IDL生成器输出目录
set(DAS_IDL_ABI_OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/abi")
set(DAS_IDL_WRAPPER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/wrapper")
set(DAS_IDL_IMPLEMENTS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/wrapper")
set(DAS_IDL_SWIG_OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/swig")
set(DAS_IDL_BATCH_CONFIG "${CMAKE_BINARY_DIR}/das/idl_batch_config.json")

# 在CMake中生成IDL批量配置JSON文件
set(DAS_IDL_JSON_CONFIG "[\n")
set(IS_FIRST_ITEM TRUE)

foreach (IDL_FILE ${DAS_IDL_FILES})
    if (IS_FIRST_ITEM)
        set(IS_FIRST_ITEM FALSE)
    else ()
        string(APPEND DAS_IDL_JSON_CONFIG ",\n")
    endif ()

    get_filename_component(IDL_FILENAME "${IDL_FILE}" NAME_WE)
    set(IDL_ABI_SUBDIR "${DAS_IDL_ABI_OUTPUT_DIR}")
    set(IDL_WRAPPER_SUBDIR "${DAS_IDL_WRAPPER_OUTPUT_DIR}")
    set(IDL_IMPLEMENTS_SUBDIR "${DAS_IDL_IMPLEMENTS_OUTPUT_DIR}")
    set(IDL_SWIG_SUBDIR "${DAS_IDL_SWIG_OUTPUT_DIR}")

    string(APPEND DAS_IDL_JSON_CONFIG "    {\n")
    string(APPEND DAS_IDL_JSON_CONFIG "        \"-i\": \"${IDL_FILE}\",\n")
    string(APPEND DAS_IDL_JSON_CONFIG "        \"--raw-output-dir\": \"${IDL_ABI_SUBDIR}\",\n")
    string(APPEND DAS_IDL_JSON_CONFIG "        \"--wrapper-output-dir\": \"${IDL_WRAPPER_SUBDIR}\",\n")
    string(APPEND DAS_IDL_JSON_CONFIG "        \"--implements-output-dir\": \"${IDL_IMPLEMENTS_SUBDIR}\",\n")
    string(APPEND DAS_IDL_JSON_CONFIG "        \"--swig-output-dir\": \"${IDL_SWIG_SUBDIR}\",\n")
    string(APPEND DAS_IDL_JSON_CONFIG "        \"--swig\": true,\n")
    string(APPEND DAS_IDL_JSON_CONFIG "        \"--cpp-wrapper\": true,\n")
    string(APPEND DAS_IDL_JSON_CONFIG "        \"--cpp-implements\": true,\n")
    string(APPEND DAS_IDL_JSON_CONFIG "        \"--generate-type-maps\": true\n")
    string(APPEND DAS_IDL_JSON_CONFIG "    }")
endforeach ()

string(APPEND DAS_IDL_JSON_CONFIG "\n]")

# 将JSON配置写入文件
file(WRITE "${DAS_IDL_BATCH_CONFIG}" "${DAS_IDL_JSON_CONFIG}")
message(STATUS "Generated IDL batch configuration: ${DAS_IDL_BATCH_CONFIG}")

# 设置更新列表文件路径
set(DAS_IDL_UPDATE_LIST "${CMAKE_BINARY_DIR}/das/idl_update_list.txt")

# IDL增量检查命令（检查哪些IDL需要重新生成）
add_custom_command(
        OUTPUT ${DAS_IDL_UPDATE_LIST}
        COMMAND ${DAS_IDL_VENV_PYTHON} ${CMAKE_SOURCE_DIR}/tools/das_idl/check_idl_updates.py
        --config ${DAS_IDL_BATCH_CONFIG}
        --output ${DAS_IDL_UPDATE_LIST}
        DEPENDS ${DAS_IDL_FILES} ${DAS_IDL_BATCH_CONFIG}
        ${DAS_IDL_MODULE_TOOLS}
        COMMENT "Check which IDL files need to be regenerated"
        VERBATIM
)

# 创建IDL增量检查目标
add_custom_target(DasCoreIdlUpdateList DEPENDS ${DAS_IDL_UPDATE_LIST})

# 执行IDL批量生成的自定义命令（使用虚拟环境，只生成需要更新的IDL）
add_custom_command(
        OUTPUT ${DAS_IDL_ABI_OUTPUT_DIR}/.generated_stamp
        COMMAND ${DAS_IDL_VENV_PYTHON} ${CMAKE_SOURCE_DIR}/tools/das_idl/das_idl_batch_gen.py
        --config ${DAS_IDL_BATCH_CONFIG}
        --update-list ${DAS_IDL_UPDATE_LIST}
        COMMAND ${CMAKE_COMMAND} -E touch ${DAS_IDL_ABI_OUTPUT_DIR}/.generated_stamp
        DEPENDS ${DAS_IDL_UPDATE_LIST}
        ${DAS_IDL_MODULE_TOOLS}
        COMMENT "Generate code from IDL files using virtual environment (incremental)"
        VERBATIM
)

# 创建IDL批量生成目标
add_custom_target(DasCoreIdlGenerated DEPENDS ${DAS_IDL_ABI_OUTPUT_DIR}/.generated_stamp)
add_dependencies(DasCoreIdlGenerated DasCoreIdlUpdateList)

# ====== IDL依赖关系提取与拓扑排序 ======
# 设置依赖关系文件输出路径（放在构建目录中，不污染源码目录）
set(DAS_SWIG_DEPS_FILE "${CMAKE_BINARY_DIR}/das/swig_deps.json")
set(DAS_SORTED_INTERFACES_FILE "${CMAKE_BINARY_DIR}/das/sorted_interfaces.txt")

# 提取IDL依赖关系的自定义命令
add_custom_command(
        OUTPUT ${DAS_SWIG_DEPS_FILE}
        COMMAND ${DAS_IDL_VENV_PYTHON} ${CMAKE_SOURCE_DIR}/tools/das_idl/extract_idl_deps.py
        --idl-dir ${CMAKE_SOURCE_DIR}/idl
        --output ${DAS_SWIG_DEPS_FILE}
        DEPENDS ${DAS_IDL_FILES}
        ${CMAKE_SOURCE_DIR}/tools/das_idl/extract_idl_deps.py
        COMMENT "Extract IDL interface dependencies"
        VERBATIM
)

# 创建IDL依赖提取目标
add_custom_target(DasCoreIdlDepsExtracted DEPENDS ${DAS_SWIG_DEPS_FILE})
add_dependencies(DasCoreIdlDepsExtracted DasCoreIdlGenerated)

# 执行拓扑排序的自定义命令
add_custom_command(
        OUTPUT ${DAS_SORTED_INTERFACES_FILE}
        COMMAND ${DAS_IDL_VENV_PYTHON} ${CMAKE_SOURCE_DIR}/tools/das_idl/topological_sort.py
        --deps-file ${DAS_SWIG_DEPS_FILE}
        --output ${DAS_SORTED_INTERFACES_FILE}
        DEPENDS ${DAS_SWIG_DEPS_FILE}
        ${CMAKE_SOURCE_DIR}/tools/das_idl/topological_sort.py
        COMMENT "Topological sort of SWIG interfaces"
        VERBATIM
)

# 创建拓扑排序目标
add_custom_target(DasCoreSwigInterfacesSorted DEPENDS ${DAS_SORTED_INTERFACES_FILE})
add_dependencies(DasCoreSwigInterfacesSorted DasCoreIdlDepsExtracted)
# ====== IDL依赖关系提取与拓扑排序结束 ======

# 设置 swig_all.i 文件路径
set(DAS_SWIG_ALL_I "${DAS_IDL_SWIG_OUTPUT_DIR}/swig_all.i")

# 生成swig_all.i文件的自定义命令（汇总所有SWIG接口）
add_custom_command(
        OUTPUT "${DAS_SWIG_ALL_I}"
        COMMAND ${CMAKE_COMMAND}
        -DSWIG_OUTPUT_DIR=${DAS_IDL_SWIG_OUTPUT_DIR}
        -DABI_OUTPUT_DIR=${DAS_IDL_ABI_OUTPUT_DIR}
        -DSORTED_INTERFACES_FILE=${DAS_SORTED_INTERFACES_FILE}
        -P ${CMAKE_SOURCE_DIR}/cmake/generate_swig_all_i.cmake
        COMMAND ${CMAKE_COMMAND} -E touch ${DAS_IDL_SWIG_OUTPUT_DIR}/.swig_all_generated
        DEPENDS ${DAS_SORTED_INTERFACES_FILE}
        ${CMAKE_SOURCE_DIR}/cmake/generate_swig_all_i.cmake
        BYPRODUCTS ${DAS_IDL_SWIG_OUTPUT_DIR}/.swig_all_generated
        COMMENT "Generate SWIG _all.i summary file"
        VERBATIM
)

# 创建SWIG汇总文件生成目标
add_custom_target(DasCoreSwigAllGenerated DEPENDS "${DAS_SWIG_ALL_I}")
add_dependencies(DasCoreSwigAllGenerated DasCoreSwigInterfacesSorted)

# 让预构建目标依赖IDL生成目标，确保增量检查脚本被执行
add_dependencies(DasCoreObjectsPreBuild DasCoreIdlUpdateList DasCoreIdlGenerated DasCoreSwigAllGenerated)

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

target_include_directories(DasCoreObjects PUBLIC ${CMAKE_BINARY_DIR}/das/include/)
target_include_directories(DasCoreObjects PUBLIC ${DAS_IDL_ABI_OUTPUT_DIR})
target_include_directories(DasCoreObjects PUBLIC ${DAS_IDL_WRAPPER_OUTPUT_DIR})
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

# 配置导出库
set(DAS_EXPORT_MACRO_LIST "")
set(DAS_FOREIGN_LANGUAGE_LIB_LIST "")

cmake_policy(SET CMP0078 NEW)

file(GLOB DAS_SWIG_FILES ${CMAKE_SOURCE_DIR}/SWIG/*.i)
set_property(SOURCE ${DAS_SWIG_FILES} PROPERTY CPLUSPLUS ON)
set_source_files_properties(${DAS_SWIG_FILES} PROPERTIES USE_SWIG_DEPENDENCIES TRUE)

if (${EXPORT_CSHARP})
    # 显式添加对 swig_all.i 的依赖
    set(SWIG_MODULE_DasCoreCSharpExport_EXTRA_DEPS "${DAS_SWIG_ALL_I}")
    das_add_swig_export_library(CSharp DasCoreCSharpExport "${DAS_SWIG_FILES}")
    target_link_libraries(DasCoreCSharpExport PUBLIC DasCore)
    add_dependencies(DasCoreCSharpExport DasCore)
    # 确保SWIG导出库在swig_all.i重新生成后重新构建
    add_dependencies(DasCoreCSharpExport DasCoreSwigAllGenerated)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_CSHARP)
endif ()

if (${EXPORT_JAVA})
    find_package(JNI REQUIRED)
    # 显式添加对 swig_all.i 的依赖
    set(SWIG_MODULE_DasCoreJavaExport_EXTRA_DEPS "${DAS_SWIG_ALL_I}")
    das_add_swig_export_library(Java DasCoreJavaExport "${DAS_SWIG_FILES}")
    set_property(TARGET DasCoreJavaExport PROPERTY SWIG_COMPILE_OPTIONS -package org.das)

    target_link_libraries(DasCoreObjects PUBLIC JNI::JNI)

    target_link_libraries(DasCoreJavaExport PUBLIC DasCore)
    add_dependencies(DasCoreJavaExport DasCore)
    # 确保SWIG导出库在swig_all.i重新生成后重新构建
    add_dependencies(DasCoreJavaExport DasCoreSwigAllGenerated)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_JAVA)
endif ()

if (${EXPORT_PYTHON})
    find_package(Python3 COMPONENTS Development REQUIRED)
    # 显式添加对 swig_all.i 的依赖
    set(SWIG_MODULE_DasCorePythonExport_EXTRA_DEPS "${DAS_SWIG_ALL_I}")
    das_add_swig_export_library(Python DasCorePythonExport "${DAS_SWIG_FILES}")
    set_property(TARGET DasCorePythonExport PROPERTY SWIG_COMPILE_OPTIONS -threads)

    target_include_directories(DasCoreObjects PUBLIC ${Python3_INCLUDE_DIRS})
    target_link_libraries(DasCoreObjects PUBLIC Python3::Python)

    target_link_libraries(DasCorePythonExport PUBLIC DasCore)
    add_dependencies(DasCorePythonExport DasCore)
    # 确保SWIG导出库在swig_all.i重新生成后重新构建
    add_dependencies(DasCorePythonExport DasCoreSwigAllGenerated)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_PYTHON)
endif ()

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
