# DasAddIdlExport.cmake
# 通用的 IDL 导出函数，用于生成 ABI、Wrapper、SWIG 接口和 IPC 代码
#
# 功能:
#   - 当 LANGUAGES 为空时：生成 ABI + wrapper + IPC 代码，不执行 find_package(SWIG)
#   - 当 LANGUAGES 非空时：生成 ABI + wrapper + SWIG 接口 + 目标语言绑定 + IPC 代码
#   - IPC 代码默认生成到 ${OUTPUT_DIR}/_autogen/idl/ipc/ 目录
#   - C++ 头文件生成到 ${OUTPUT_DIR}/_autogen/idl/header/ 目录
#   - 支持增量检查和批量生成，提高构建效率
#   - 支持用户自定义 SWIG 文件，通过 USER_SWIG_FILES 参数指定
#
# 重要概念:
#   - 代码生成器会生成 SWIG 接口文件 (*.swig.i)
#   - 用户自定义的 SWIG 文件通过 %include 指令包含自动生成的代码
#   - 示例: 在用户自定义文件中添加 %include <das/_autogen/idl/swig/swig_all.i>
#
# 用法示例:
#   # 仅生成 C++ 代码和 IPC（不生成 SWIG 绑定，适用于测试 IDL）
#   das_add_idl_export(
#       NAME DasTest
#       IDL_DIR "${CMAKE_SOURCE_DIR}/das/test/idl"
#       OUTPUT_DIR "${CMAKE_BINARY_DIR}/das_test"
#       IDL_FILES "ITest1.idl" "ITest2.idl"
#       LANGUAGES ""  # 不需要 SWIG 导出
#   )
#
#   # 生成 C++ 代码 + SWIG 绑定 + IPC（使用用户自定义 SWIG 文件）
#   das_add_idl_export(
#       NAME DasCore
#       IDL_DIR "${CMAKE_SOURCE_DIR}/idl"
#       OUTPUT_DIR "${CMAKE_BINARY_DIR}/das"
#       IDL_FILES "IDasBase.idl" "IDasLogger.idl"
#       LANGUAGES Python Java CSharp
#       USER_SWIG_FILES "${CMAKE_SOURCE_DIR}/SWIG/ExportAll.i"
#   )
#
#   # 仅生成 C++ 代码，不生成 IPC
#   das_add_idl_export(
#       NAME DasMinimal
#       IDL_DIR "${CMAKE_SOURCE_DIR}/idl"
#       OUTPUT_DIR "${CMAKE_BINARY_DIR}/das"
#       IDL_FILES "ISimple.idl"
#       GENERATE_IPC_PROXY OFF
#       GENERATE_IPC_STUB OFF
#   )
#
# 生成的目标:
#   - ${NAME}IdlUpdateList: 增量检查目标，生成需要更新的 IDL 列表
#   - ${NAME}IdlHeadersGenerated: 头文件生成目标，生成 .generated.h 文件
#   - ${NAME}IdlGenerated: 批量生成目标，执行 IDL 代码生成
#   - ${NAME}IdlDepsExtracted: (仅当 LANGUAGES 非空) 依赖提取目标
#   - ${NAME}SwigInterfacesSorted: (仅当 LANGUAGES 非空) 拓扑排序目标
#
# 输出变量:
#   - ${NAME}_GENERATED_FILES: 所有生成的文件列表
#   - ${NAME}_GENERATED_ABI_FILES: ABI 文件列表
#   - ${NAME}_GENERATED_WRAPPER_FILES: Wrapper 文件列表
#   - ${NAME}_GENERATED_SWIG_FILES: SWIG 文件列表（代码生成器生成）
#   - ${NAME}_USER_SWIG_FILES: 用户指定的 SWIG 文件列表
#   - ${NAME}_EXPORT_DEFINITIONS: 导出宏定义列表（如 DAS_EXPORT_PYTHON, DAS_EXPORT_JAVA, DAS_EXPORT_CSHARP）
#   - ${NAME}_HEADER_OUTPUT_DIR: 头文件输出目录
#   - ${NAME}_GENERATED_FILES: 所有生成的文件列表
#   - ${NAME}_GENERATED_ABI_FILES: ABI 文件列表
#   - ${NAME}_GENERATED_WRAPPER_FILES: Wrapper 文件列表
#   - ${NAME}_GENERATED_SWIG_FILES: SWIG 文件列表（代码生成器生成）
#   - ${NAME}_USER_SWIG_FILES: 用户指定的 SWIG 文件列表

# 引入依赖模块
if(NOT COMMAND das_idl_generate)
    include("${CMAKE_CURRENT_LIST_DIR}/DasIdlGenerator.cmake")
endif()

# 引入 SWIG 导出库函数（当需要语言绑定时使用）
if(NOT COMMAND das_add_swig_export_library)
    include("${CMAKE_CURRENT_LIST_DIR}/DasUtils.cmake")
endif()

# ============================================
# 语言绑定输出路径变量
# ============================================
# 语言绑定输出路径变量
# ============================================
if(NOT DEFINED DAS_LANGUAGE_BIN_DIR)
    set(DAS_LANGUAGE_BIN_DIR "${CMAKE_BINARY_DIR}/bin/das"
        CACHE PATH "DAS language bindings output directory")
endif()


# das_add_idl_export 函数
# 参数:
#   NAME                  - 库名称 (如 DasCore, DasTest)，用于目标命名
#   IDL_DIR               - IDL 源文件目录
#   OUTPUT_DIR            - 用户指定的输出根目录
#   IDL_FILES             - 要处理的 IDL 文件列表（文件名，不含路径）
#   LANGUAGES             - 要导出的语言列表 (Python, Java, CSharp)，可为空
#                         - 为空时只生成 C++ 代码和 IPC，不调用 SWIG
#   USER_SWIG_FILES       - 用户自定义的 SWIG 文件列表（可选）
#                         - 用于覆盖代码生成器生成的 SWIG 文件
#                         - 用户文件中应通过 %include 包含自动生成的 swig_all.i
#   GENERATE_IPC_PROXY    - 是否生成 IPC Proxy 代码，默认 ON
#   GENERATE_IPC_STUB     - 是否生成 IPC Stub 代码，默认 ON
#   NAMESPACE             - 可选的命名空间
#   SWIG_OPTIONS_Python   - Python 的 SWIG 编译选项（可选）
#   SWIG_OPTIONS_Java     - Java 的 SWIG 编译选项（可选）
#   SWIG_OPTIONS_CSharp   - CSharp 的 SWIG 编译选项（可选）
#   SWIG_INCLUDE_DIRS     - 额外的 SWIG 包含目录列表（可选，如 ${CMAKE_SOURCE_DIR}/include/ 等）
#   DEPENDS_ON            - SWIG 导出库依赖的其他目标列表（可选，如 DasCore）
#   LUA_OPEN_MODULE_NAME  - Lua C API open symbol suffix, required when LANGUAGES contains Lua
#   NODE_PACKAGE_NAME     - Node public package identity, required when LANGUAGES contains Node
#   NODE_ADDON_NAME       - Node native addon basename, required when LANGUAGES contains Node
#
# 输出变量 (通过 PARENT_SCOPE 输出):
#   ${NAME}_GENERATED_FILES        - 所有生成的文件列表
#   ${NAME}_GENERATED_ABI_FILES    - ABI 文件列表
#   ${NAME}_GENERATED_WRAPPER_FILES - Wrapper 文件列表
#   ${NAME}_GENERATED_SWIG_FILES   - SWIG 文件列表（代码生成器生成）
#   ${NAME}_USER_SWIG_FILES        - 用户指定的 SWIG 文件列表
#   ${NAME}_EXPORT_DEFINITIONS     - 导出宏定义列表（如 DAS_EXPORT_PYTHON, DAS_EXPORT_JAVA, DAS_EXPORT_CSHARP）
#   ${NAME}_GENERATED_FILES        - 所有生成的文件列表
#   ${NAME}_GENERATED_ABI_FILES    - ABI 文件列表
#   ${NAME}_GENERATED_WRAPPER_FILES - Wrapper 文件列表
#   ${NAME}_GENERATED_SWIG_FILES   - SWIG 文件列表（代码生成器生成）
#   ${NAME}_USER_SWIG_FILES        - 用户指定的 SWIG 文件列表
function(das_add_idl_export)
    cmake_parse_arguments(
        DAS_IDL_EXPORT                          # 前缀
        ""                                      # 选项 (无值参数)
        "NAME;IDL_DIR;OUTPUT_DIR;NAMESPACE;GENERATE_IPC_PROXY;GENERATE_IPC_STUB;IPC_CACHE_DIR;SWIG_MODULE_NAME;EXPORT_MACRO;EXPORT_C_MACRO;LUA_OPEN_MODULE_NAME;NODE_PACKAGE_NAME;NODE_ADDON_NAME"  # 单值参数
        "IDL_FILES;LANGUAGES;USER_SWIG_FILES;GENERATED_FILES;GENERATED_ABI_FILES;GENERATED_WRAPPER_FILES;GENERATED_SWIG_FILES;GENERATED_IPC_FILES;SWIG_OPTIONS_Python;SWIG_OPTIONS_Java;SWIG_OPTIONS_CSharp;SWIG_INCLUDE_DIRS;DEPENDS_ON"  # 多值参数
        ${ARGN}
    )

    # ====== 参数验证 ======
    if(NOT DAS_IDL_EXPORT_NAME)
        message(FATAL_ERROR "[das_add_idl_export] NAME is required")
    endif()

    if(NOT DAS_IDL_EXPORT_IDL_DIR)
        message(FATAL_ERROR "[das_add_idl_export] IDL_DIR is required")
    endif()

    if(NOT DAS_IDL_EXPORT_OUTPUT_DIR)
        message(FATAL_ERROR "[das_add_idl_export] OUTPUT_DIR is required")
    endif()

    if(NOT DAS_IDL_EXPORT_IDL_FILES)
        message(FATAL_ERROR "[das_add_idl_export] IDL_FILES is required")
    endif()

    # ====== 设置默认值 ======
    # IPC 选项默认为 ON
    if(NOT DEFINED DAS_IDL_EXPORT_GENERATE_IPC_PROXY)
        set(DAS_IDL_EXPORT_GENERATE_IPC_PROXY ON)
    endif()
    if(NOT DEFINED DAS_IDL_EXPORT_GENERATE_IPC_STUB)
        set(DAS_IDL_EXPORT_GENERATE_IPC_STUB ON)
    endif()

    # ====== 设置输出目录结构 ======
    # 兼容现有体系: ${OUTPUT_DIR}/_autogen/idl/{abi,wrapper,swig,ipc,iids,header}
    set(_ABI_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/abi")
    set(_WRAPPER_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/wrapper")
    set(_SWIG_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/swig")
    set(_IPC_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/ipc")
    if(NOT DAS_IDL_EXPORT_IPC_CACHE_DIR)
        set(DAS_IDL_EXPORT_IPC_CACHE_DIR "${_IPC_OUTPUT_DIR}/cache")
    endif()
    set(_IPC_CACHE_DIR "${DAS_IDL_EXPORT_IPC_CACHE_DIR}")
    set(_IIDS_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/iids")
    set(_HEADER_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/header")
    set(_IPC_GENERATED_CPP "${_IPC_OUTPUT_DIR}/IpcGenerated.cpp")

    message(STATUS "[das_add_idl_export] Configuring IDL export for ${DAS_IDL_EXPORT_NAME}")
    message(STATUS "[das_add_idl_export]   IDL_DIR: ${DAS_IDL_EXPORT_IDL_DIR}")
    message(STATUS "[das_add_idl_export]   OUTPUT_DIR: ${DAS_IDL_EXPORT_OUTPUT_DIR}")
    message(STATUS "[das_add_idl_export]   IDL_FILES: ${DAS_IDL_EXPORT_IDL_FILES}")
    if(DAS_IDL_EXPORT_LANGUAGES)
        message(STATUS "[das_add_idl_export]   LANGUAGES: ${DAS_IDL_EXPORT_LANGUAGES}")
    else()
        message(STATUS "[das_add_idl_export]   LANGUAGES: (none - no SWIG bindings)")
    endif()
    if(DAS_IDL_EXPORT_USER_SWIG_FILES)
        message(STATUS "[das_add_idl_export]   USER_SWIG_FILES: ${DAS_IDL_EXPORT_USER_SWIG_FILES}")
    endif()
    message(STATUS "[das_add_idl_export]   IPC_PROXY: ${DAS_IDL_EXPORT_GENERATE_IPC_PROXY}")
    message(STATUS "[das_add_idl_export]   IPC_STUB: ${DAS_IDL_EXPORT_GENERATE_IPC_STUB}")

    # ====== 判断是否需要 SWIG ======
    set(_NEED_SWIG FALSE)
    if(DAS_IDL_EXPORT_LANGUAGES)
        foreach(_LANG ${DAS_IDL_EXPORT_LANGUAGES})
            string(TOLOWER "${_LANG}" _LANG_LOWER)
            if(_LANG_LOWER STREQUAL "python"
                OR _LANG_LOWER STREQUAL "java"
                OR _LANG_LOWER STREQUAL "csharp")
                set(_NEED_SWIG TRUE)
                break()
            endif()
        endforeach()
    endif()

    # ====== 确保 Python 虚拟环境已设置 ======
    das_idl_setup_venv()

    # ====== 收集所有 IDL 文件的完整路径 ======
    set(_FULL_IDL_PATHS "")
    foreach(IDL_FILE_NAME ${DAS_IDL_EXPORT_IDL_FILES})
        list(APPEND _FULL_IDL_PATHS "${DAS_IDL_EXPORT_IDL_DIR}/${IDL_FILE_NAME}")
    endforeach()

    # ====== 使用 GLOB 自动收集 das_idl 目录下的所有 Python 脚本 ======
    file(GLOB _DAS_IDL_MODULE_TOOLS "${CMAKE_SOURCE_DIR}/tools/das_idl/*.py")

    # ====== 设置中间文件路径 ======
    set(_BATCH_CONFIG_FILE "${DAS_IDL_EXPORT_OUTPUT_DIR}/${DAS_IDL_EXPORT_NAME}_idl_batch_config.json")
    set(_UPDATE_LIST_FILE "${DAS_IDL_EXPORT_OUTPUT_DIR}/${DAS_IDL_EXPORT_NAME}_idl_update_list.txt")
    set(_SWIG_DEPS_FILE "${DAS_IDL_EXPORT_OUTPUT_DIR}/${DAS_IDL_EXPORT_NAME}_swig_deps.json")
    set(_SORTED_INTERFACES_FILE "${DAS_IDL_EXPORT_OUTPUT_DIR}/${DAS_IDL_EXPORT_NAME}_sorted_interfaces.txt")

    # ====== 设置 stamp 目录和文件路径 ======
    set(_DAS_IDL_STAMP_DIR "${CMAKE_BINARY_DIR}/stamps/das_idl")
    set(_UPDATE_LIST_STAMP "${_DAS_IDL_STAMP_DIR}/${DAS_IDL_EXPORT_NAME}_idl_update_list.stamp")
    set(_GENERATED_STAMP "${_DAS_IDL_STAMP_DIR}/${DAS_IDL_EXPORT_NAME}_idl_generated.stamp")
    set(_SWIG_DEPS_STAMP "${_DAS_IDL_STAMP_DIR}/${DAS_IDL_EXPORT_NAME}_swig_deps.stamp")
    set(_SORTED_STAMP "${_DAS_IDL_STAMP_DIR}/${DAS_IDL_EXPORT_NAME}_sorted_interfaces.stamp")
    set(_SWIG_ALL_STAMP "${_DAS_IDL_STAMP_DIR}/${DAS_IDL_EXPORT_NAME}_swig_all.stamp")

    # ====== 1. 生成批量配置 JSON 文件 ======
    # 检测 LANGUAGES 中是否包含 Lua（大小写不敏感），设置 Lua 输出目录
    set(_HAS_LUA FALSE)
    set(_LUA_OUTPUT_DIR "")
    foreach(_LANG ${DAS_IDL_EXPORT_LANGUAGES})
        string(TOLOWER "${_LANG}" _LANG_LOWER)
        if(_LANG_LOWER STREQUAL "lua")
            set(_HAS_LUA TRUE)
            set(_LUA_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/lua")
            file(MAKE_DIRECTORY ${_LUA_OUTPUT_DIR})
            break()
        endif()
    endforeach()

    if(_HAS_LUA AND NOT DAS_IDL_EXPORT_LUA_OPEN_MODULE_NAME)
        message(FATAL_ERROR "[das_add_idl_export] LUA_OPEN_MODULE_NAME is required when LANGUAGES contains Lua")
    endif()

    # 检测 LANGUAGES 中是否包含 Node（大小写不敏感），设置 Node 输出目录
    set(_HAS_NODE FALSE)
    set(_NODE_OUTPUT_DIR "")
    foreach(_LANG ${DAS_IDL_EXPORT_LANGUAGES})
        string(TOLOWER "${_LANG}" _LANG_LOWER)
        if(_LANG_LOWER STREQUAL "node")
            set(_HAS_NODE TRUE)
            set(_NODE_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/node")
            file(MAKE_DIRECTORY ${_NODE_OUTPUT_DIR})
            break()
        endif()
    endforeach()

    if(_HAS_NODE AND NOT DAS_IDL_EXPORT_NODE_PACKAGE_NAME)
        message(FATAL_ERROR "[das_add_idl_export] NODE_PACKAGE_NAME is required when LANGUAGES contains Node")
    endif()

    if(_HAS_NODE AND NOT DAS_IDL_EXPORT_NODE_ADDON_NAME)
        message(FATAL_ERROR "[das_add_idl_export] NODE_ADDON_NAME is required when LANGUAGES contains Node")
    endif()

    # 使用 Python 脚本生成 JSON，替代 file(CONFIGURE OUTPUT ...) 以避免内容比对导致的不更新问题
    set(_GEN_CONFIG_SCRIPT "${CMAKE_SOURCE_DIR}/tools/das_idl/gen_batch_config.py")

    set(_GEN_CONFIG_ARGS
        --output "${_BATCH_CONFIG_FILE}"
        --idl-dir "${DAS_IDL_EXPORT_IDL_DIR}"
        --idl-files ${DAS_IDL_EXPORT_IDL_FILES}
        --raw-output-dir "${_ABI_OUTPUT_DIR}"
        --wrapper-output-dir "${_WRAPPER_OUTPUT_DIR}"
        --header-output-dir "${_HEADER_OUTPUT_DIR}"
        --export-macro "${DAS_IDL_EXPORT_EXPORT_MACRO}"
        --export-c-macro "${DAS_IDL_EXPORT_EXPORT_C_MACRO}"
    )

    # SWIG
    if(_NEED_SWIG)
        list(APPEND _GEN_CONFIG_ARGS --swig-output-dir "${_SWIG_OUTPUT_DIR}")
    endif()

    # IPC
    if(DAS_IDL_EXPORT_GENERATE_IPC_PROXY OR DAS_IDL_EXPORT_GENERATE_IPC_STUB)
        list(APPEND _GEN_CONFIG_ARGS
            --ipc-output-dir "${_IPC_OUTPUT_DIR}"
            --ipc-cache-dir "${_IPC_CACHE_DIR}"
        )
        if(DAS_IDL_EXPORT_GENERATE_IPC_PROXY)
            list(APPEND _GEN_CONFIG_ARGS --ipc-proxy)
        endif()
        if(DAS_IDL_EXPORT_GENERATE_IPC_STUB)
            list(APPEND _GEN_CONFIG_ARGS --ipc-stub)
        endif()
    endif()

    # 命名空间
    if(DAS_IDL_EXPORT_NAMESPACE)
        list(APPEND _GEN_CONFIG_ARGS --namespace "${DAS_IDL_EXPORT_NAMESPACE}")
    endif()

    # 语言列表
    if(DAS_IDL_EXPORT_LANGUAGES)
        list(APPEND _GEN_CONFIG_ARGS --languages ${DAS_IDL_EXPORT_LANGUAGES})
    endif()

    # Lua 输出目录（仅在 LANGUAGES 包含 Lua 时设置）
    if(_LUA_OUTPUT_DIR)
        list(APPEND _GEN_CONFIG_ARGS
            --lua-output-dir "${_LUA_OUTPUT_DIR}"
            --lua-name "${DAS_IDL_EXPORT_NAME}"
            --lua-open-module-name "${DAS_IDL_EXPORT_LUA_OPEN_MODULE_NAME}"
        )
    endif()

    # Node/NAPI 输出目录（仅在 LANGUAGES 包含 Node 时设置）
    if(_NODE_OUTPUT_DIR)
        list(APPEND _GEN_CONFIG_ARGS
            --node-output-dir "${_NODE_OUTPUT_DIR}"
            --node-package-name "${DAS_IDL_EXPORT_NODE_PACKAGE_NAME}"
            --node-addon-name "${DAS_IDL_EXPORT_NODE_ADDON_NAME}"
        )
    endif()

    execute_process(
        COMMAND "${DAS_IDL_VENV_PYTHON}" "${_GEN_CONFIG_SCRIPT}" ${_GEN_CONFIG_ARGS}
        RESULT_VARIABLE _GEN_CONFIG_RESULT
        ERROR_VARIABLE _GEN_CONFIG_ERROR
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools/das_idl"
    )

    if(NOT _GEN_CONFIG_RESULT EQUAL 0)
        message(FATAL_ERROR "[das_add_idl_export] Failed to generate batch config: ${_GEN_CONFIG_ERROR}")
    endif()

    message(STATUS "[das_add_idl_export] Generated IDL batch configuration: ${_BATCH_CONFIG_FILE}")

    # ====== 2. 创建增量检查命令和目标 ======
    add_custom_command(
        OUTPUT ${_UPDATE_LIST_FILE} ${_UPDATE_LIST_STAMP}
        COMMAND "${DAS_IDL_VENV_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/das_idl/check_idl_updates.py"
            --config "${_BATCH_CONFIG_FILE}"
            --output "${_UPDATE_LIST_FILE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_DAS_IDL_STAMP_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_UPDATE_LIST_STAMP}"
        DEPENDS ${_FULL_IDL_PATHS} "${_BATCH_CONFIG_FILE}"
            ${_DAS_IDL_MODULE_TOOLS}
        COMMENT "[das_add_idl_export] Check which IDL files need to be regenerated for ${DAS_IDL_EXPORT_NAME}"
        VERBATIM
    )

    set(_UPDATE_LIST_TARGET "${DAS_IDL_EXPORT_NAME}IdlUpdateList")
    if(NOT TARGET ${_UPDATE_LIST_TARGET})
        add_custom_target(${_UPDATE_LIST_TARGET}
            DEPENDS ${_UPDATE_LIST_FILE} ${_UPDATE_LIST_STAMP}
        )
    endif()

    # ====== 2b. 头文件生成现在由 batch pipeline 统一处理 ======
    # header 文件通过 --header 选项注册到 das_idl_gen.py，
    # 输出文件通过 --list-outputs 发现，从 _DYNAMIC_OUTPUT_LIST 中分类提取。
    # 不再使用独立的 foreach 循环调用 das_header_generator.py。

    # ====== 3. 创建批量生成命令和目标 ======

    # ====== 在 configure 阶段获取生成器预期输出文件列表 ======
    set(_IPC_CACHE_DIR_ARGS "")
    if(_IPC_CACHE_DIR)
        set(_IPC_CACHE_DIR_ARGS "--ipc-cache-dir" "${_IPC_CACHE_DIR}")
    endif()

    execute_process(
        COMMAND "${DAS_IDL_VENV_PYTHON}"
            "${CMAKE_SOURCE_DIR}/tools/das_idl/das_idl_batch_gen.py"
            --config "${_BATCH_CONFIG_FILE}"
            --list-outputs
            ${_IPC_CACHE_DIR_ARGS}
        OUTPUT_VARIABLE _DYNAMIC_OUTPUT_LIST
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _LIST_RESULT
        ERROR_VARIABLE _LIST_ERROR
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools/das_idl"
    )

    if(NOT _LIST_RESULT EQUAL 0)
        message(WARNING "[das_add_idl_export] Failed to list expected outputs: ${_LIST_ERROR}")
        set(_DYNAMIC_OUTPUT_LIST "")
    endif()

    # 换行分隔 → CMake list
    string(REPLACE "\n" ";" _DYNAMIC_OUTPUT_LIST "${_DYNAMIC_OUTPUT_LIST}")
    list(FILTER _DYNAMIC_OUTPUT_LIST EXCLUDE REGEX "^$")

    # 确保 IDL 文件和生成器脚本变更触发重新 configure
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${_FULL_IDL_PATHS}
        ${_DAS_IDL_MODULE_TOOLS}
    )

    add_custom_command(
        OUTPUT ${_GENERATED_STAMP} ${_DYNAMIC_OUTPUT_LIST}
        COMMAND "${DAS_IDL_VENV_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/das_idl/das_idl_batch_gen.py"
            --config "${_BATCH_CONFIG_FILE}"
            --update-list "${_UPDATE_LIST_FILE}"
            --ipc-cache-dir "${_IPC_CACHE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_DAS_IDL_STAMP_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_GENERATED_STAMP}"
        DEPENDS ${_UPDATE_LIST_FILE}
            ${_DAS_IDL_MODULE_TOOLS}
        COMMENT "[das_add_idl_export] Generate code from IDL files (incremental) for ${DAS_IDL_EXPORT_NAME}"
        VERBATIM
    )

    set(_GENERATED_TARGET "${DAS_IDL_EXPORT_NAME}IdlGenerated")
    if(NOT TARGET ${_GENERATED_TARGET})
        add_custom_target(${_GENERATED_TARGET}
            DEPENDS ${_GENERATED_STAMP}
        )
        add_dependencies(${_GENERATED_TARGET} ${_UPDATE_LIST_TARGET})
    endif()

    # ====== 4. SWIG 依赖提取和拓扑排序 (仅当 LANGUAGES 非空时) ======
    if(_NEED_SWIG)
        # 依赖提取命令
        add_custom_command(
            OUTPUT ${_SWIG_DEPS_FILE} ${_SWIG_DEPS_STAMP}
            COMMAND "${DAS_IDL_VENV_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/das_idl/extract_idl_deps.py"
                --idl-dir "${DAS_IDL_EXPORT_IDL_DIR}"
                --output "${_SWIG_DEPS_FILE}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_DAS_IDL_STAMP_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${_SWIG_DEPS_STAMP}"
            DEPENDS ${_FULL_IDL_PATHS}
                "${CMAKE_SOURCE_DIR}/tools/das_idl/extract_idl_deps.py"
            COMMENT "[das_add_idl_export] Extract IDL interface dependencies for ${DAS_IDL_EXPORT_NAME}"
            VERBATIM
        )

        set(_DEPS_EXTRACTED_TARGET "${DAS_IDL_EXPORT_NAME}IdlDepsExtracted")
        if(NOT TARGET ${_DEPS_EXTRACTED_TARGET})
            add_custom_target(${_DEPS_EXTRACTED_TARGET}
                DEPENDS ${_SWIG_DEPS_FILE} ${_SWIG_DEPS_STAMP}
            )
            add_dependencies(${_DEPS_EXTRACTED_TARGET} ${_GENERATED_TARGET})
        endif()

        # 拓扑排序命令
        add_custom_command(
            OUTPUT ${_SORTED_INTERFACES_FILE} ${_SORTED_STAMP}
            COMMAND "${DAS_IDL_VENV_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/das_idl/topological_sort.py"
                --deps-file "${_SWIG_DEPS_FILE}"
                --output "${_SORTED_INTERFACES_FILE}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_DAS_IDL_STAMP_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${_SORTED_STAMP}"
            DEPENDS ${_SWIG_DEPS_FILE}
                "${CMAKE_SOURCE_DIR}/tools/das_idl/topological_sort.py"
            COMMENT "[das_add_idl_export] Topological sort of SWIG interfaces for ${DAS_IDL_EXPORT_NAME}"
            VERBATIM
        )

        set(_SORTED_TARGET "${DAS_IDL_EXPORT_NAME}SwigInterfacesSorted")
        if(NOT TARGET ${_SORTED_TARGET})
            add_custom_target(${_SORTED_TARGET}
                DEPENDS ${_SORTED_INTERFACES_FILE} ${_SORTED_STAMP}
            )
            add_dependencies(${_SORTED_TARGET} ${_DEPS_EXTRACTED_TARGET})
        endif()

        # 设置 swig_all.i 文件路径
        set(_SWIG_ALL_I "${_SWIG_OUTPUT_DIR}/swig_all.i")

        # 生成 swig_all.i 文件（汇总所有 SWIG 接口）
        add_custom_command(
            OUTPUT "${_SWIG_ALL_I}" ${_SWIG_ALL_STAMP}
            COMMAND ${CMAKE_COMMAND}
                -DSWIG_OUTPUT_DIR=${_SWIG_OUTPUT_DIR}
                -DABI_OUTPUT_DIR=${_ABI_OUTPUT_DIR}
                -DSORTED_INTERFACES_FILE=${_SORTED_INTERFACES_FILE}
                -P ${CMAKE_SOURCE_DIR}/cmake/generate_swig_all_i.cmake
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_DAS_IDL_STAMP_DIR}"
            COMMAND ${CMAKE_COMMAND} -E touch "${_SWIG_ALL_STAMP}"
            DEPENDS
                ${_SORTED_INTERFACES_FILE}
                ${CMAKE_SOURCE_DIR}/cmake/generate_swig_all_i.cmake
            COMMENT "Generate SWIG swig_all.i summary file for ${DAS_IDL_EXPORT_NAME}"
            VERBATIM
        )

        # 创建 swig_all.i 生成目标
        set(_SWIG_ALL_TARGET "${DAS_IDL_EXPORT_NAME}SwigAllGenerated")
        if(NOT TARGET ${_SWIG_ALL_TARGET})
            add_custom_target(${_SWIG_ALL_TARGET}
                DEPENDS "${_SWIG_ALL_I}" ${_SWIG_ALL_STAMP}
            )
            add_dependencies(${_SWIG_ALL_TARGET} ${_SORTED_TARGET})
        endif()
    endif()

    # ====== 从动态列表中分类生成文件（全部来自 --list-outputs） ======
    set(_ALL_ABI_FILES "")
    set(_ALL_WRAPPER_FILES "")
    set(_ALL_SWIG_FILES "")
    set(_ALL_IPC_FILES "")
    set(_ALL_HEADER_FILES "")
    set(_ALL_LUA_FILES "")
    set(_ALL_NODE_FILES "")
    set(_ALL_GENERATED_FILES ${_DYNAMIC_OUTPUT_LIST})

    foreach(_FILE ${_DYNAMIC_OUTPUT_LIST})
        if(_FILE MATCHES "/abi/")
            list(APPEND _ALL_ABI_FILES "${_FILE}")
        elseif(_FILE MATCHES "/wrapper/")
            list(APPEND _ALL_WRAPPER_FILES "${_FILE}")
        elseif(_FILE MATCHES "/swig/")
            list(APPEND _ALL_SWIG_FILES "${_FILE}")
        elseif(_FILE MATCHES "/ipc/")
            list(APPEND _ALL_IPC_FILES "${_FILE}")
        elseif(_FILE MATCHES "/header/")
            list(APPEND _ALL_HEADER_FILES "${_FILE}")
        elseif(_FILE MATCHES "/lua/")
            list(APPEND _ALL_LUA_FILES "${_FILE}")
        elseif(_FILE MATCHES "/node/")
            list(APPEND _ALL_NODE_FILES "${_FILE}")
        endif()
    endforeach()

    # 创建头文件生成目标（从 dynamic list 分类而来，其他目标可依赖此目标）
    set(_HEADER_GENERATED_TARGET "${DAS_IDL_EXPORT_NAME}IdlHeadersGenerated")
    if(NOT TARGET ${_HEADER_GENERATED_TARGET})
        add_custom_target(${_HEADER_GENERATED_TARGET}
            DEPENDS ${_ALL_HEADER_FILES}
        )
        add_dependencies(${_HEADER_GENERATED_TARGET} ${_GENERATED_TARGET})
    endif()

    # ====== 如果需要 SWIG 绑定，创建 SWIG 导出库 ======
    if(_NEED_SWIG)
        # 查找 SWIG（仅在需要时）
        find_package(SWIG REQUIRED)
        cmake_policy(SET CMP0078 NEW)

        # 为每个语言创建导出库
        foreach(_LANG ${DAS_IDL_EXPORT_LANGUAGES})
            # 标准化语言名称
            string(TOLOWER "${_LANG}" _LANG_LOWER)
            if(_LANG_LOWER STREQUAL "python")
                set(_LANG_NAME "Python")
            elseif(_LANG_LOWER STREQUAL "java")
                set(_LANG_NAME "Java")
            elseif(_LANG_LOWER STREQUAL "csharp")
                set(_LANG_NAME "CSharp")
            elseif(_LANG_LOWER STREQUAL "lua")
                # Lua uses sol2 path, not SWIG — handled separately below
                continue()
            elseif(_LANG_LOWER STREQUAL "node")
                # Node uses NAPI reduce path, not SWIG — handled separately below
                continue()
            else()
                message(WARNING "[das_add_idl_export] Unsupported language: ${_LANG}")
                continue()
            endif()

            # 构建导出库名称
            set(_EXPORT_LIB_NAME "${DAS_IDL_EXPORT_NAME}${_LANG_NAME}Export")

            message(STATUS "[das_add_idl_export] Creating ${_LANG_NAME} export library: ${_EXPORT_LIB_NAME}")

            # 检查是否需要特定语言的依赖
            if(_LANG_NAME STREQUAL "Java")
                find_package(JNI REQUIRED)
            elseif(_LANG_NAME STREQUAL "Python")
                find_package(Python3 COMPONENTS Development Development.SABIModule REQUIRED)
            endif()

            # 调用 das_add_swig_export_library 创建导出库
            # 如果用户指定了自定义 SWIG 文件，则使用用户的；否则使用代码生成器生成的
            if(DAS_IDL_EXPORT_USER_SWIG_FILES)
                set(_SWIG_FILES_TO_USE "${DAS_IDL_EXPORT_USER_SWIG_FILES}")
                set(_USER_SWIG_FILES "${DAS_IDL_EXPORT_USER_SWIG_FILES}")
                message(STATUS "[das_add_idl_export]   Using user-specified SWIG files")
            else()
                set(_SWIG_FILES_TO_USE "${_ALL_SWIG_FILES}")
                set(_USER_SWIG_FILES "")
                message(STATUS "[das_add_idl_export]   Using auto-generated SWIG files")
            endif()

            # 设置 C++ 模式（必须在创建目标之前设置）
            set_property(SOURCE ${_ALL_SWIG_FILES} PROPERTY CPLUSPLUS ON)
            if(_USER_SWIG_FILES)
                set_property(SOURCE ${_USER_SWIG_FILES} PROPERTY CPLUSPLUS ON)
            endif()

            # 启用 SWIG 依赖跟踪（必须在创建目标之前设置）
            set_property(SOURCE ${_ALL_SWIG_FILES} PROPERTY USE_SWIG_DEPENDENCIES TRUE)
            if(_USER_SWIG_FILES)
                set_property(SOURCE ${_USER_SWIG_FILES} PROPERTY USE_SWIG_DEPENDENCIES TRUE)
            endif()

            # 设置 SWIG 模块名（当用户 SWIG 文件名与 %module 名不一致时必须指定）
            # 确保 CMake 正确推断输出文件名（如 DuskAutoScript.py 而非 exportall.py）
            if(DAS_IDL_EXPORT_SWIG_MODULE_NAME AND _USER_SWIG_FILES)
                set_property(SOURCE ${_USER_SWIG_FILES}
                    PROPERTY SWIG_MODULE_NAME ${DAS_IDL_EXPORT_SWIG_MODULE_NAME})
            endif()

            das_add_swig_export_library(
                ${_LANG_NAME}
                ${_EXPORT_LIB_NAME}
                "${_SWIG_FILES_TO_USE}"
            )

            # 设置 SWIG 包含目录
            set(_SWIG_INCLUDE_DIRS
                ${DAS_IDL_EXPORT_OUTPUT_DIR}
                ${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/abi
                ${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/swig
                ${_HEADER_OUTPUT_DIR})

            # 添加外部传入的包含目录
            if(DAS_IDL_EXPORT_SWIG_INCLUDE_DIRS)
                list(APPEND _SWIG_INCLUDE_DIRS ${DAS_IDL_EXPORT_SWIG_INCLUDE_DIRS})
            endif()

            set_property(
                TARGET ${_EXPORT_LIB_NAME}
                PROPERTY SWIG_INCLUDE_DIRECTORIES ${_SWIG_INCLUDE_DIRS})

            # 设置编译器包含目录（使 wrapper 文件能正确找到头文件）
            # 使用外部传入的包含目录（如果有的话）
            if(DAS_IDL_EXPORT_SWIG_INCLUDE_DIRS)
                target_include_directories(${_EXPORT_LIB_NAME} PRIVATE
                    ${_SWIG_INCLUDE_DIRS}
                )
            endif()

            # 设置额外依赖（确保 swig_all.i 被包含）
            set(SWIG_MODULE_${_EXPORT_LIB_NAME}_EXTRA_DEPS "${_SWIG_ALL_I}")

            # 设置 SWIG_COMPILE_OPTIONS（使用用户指定的选项）
            if(DAS_IDL_EXPORT_SWIG_OPTIONS_${_LANG_NAME})
                set_property(TARGET ${_EXPORT_LIB_NAME} PROPERTY SWIG_COMPILE_OPTIONS ${DAS_IDL_EXPORT_SWIG_OPTIONS_${_LANG_NAME}})
            endif()

            # 设置导出库的包含目录（追加 IDL 源文件目录）
            set_property(
                TARGET ${_EXPORT_LIB_NAME}
                APPEND PROPERTY SWIG_INCLUDE_DIRECTORIES
                    "${DAS_IDL_EXPORT_IDL_DIR}"
            )

            # 确保 SWIG 导出库在 swig_all.i 重新生成后重新构建
            add_dependencies(${_EXPORT_LIB_NAME} ${_SWIG_ALL_TARGET})

            # 链接外部依赖库（如 DasCore）
            if(DAS_IDL_EXPORT_DEPENDS_ON)
                target_link_libraries(${_EXPORT_LIB_NAME} PRIVATE ${DAS_IDL_EXPORT_DEPENDS_ON})
            endif()

            # 链接对应语言的虚拟机库
            if(_LANG_NAME STREQUAL "Python")
                target_link_libraries(${_EXPORT_LIB_NAME} PRIVATE Python3::SABIModule)
            elseif(_LANG_NAME STREQUAL "Java")
                target_link_libraries(${_EXPORT_LIB_NAME} PRIVATE JNI::JNI)
                
                # ============================================
                # Java JAR 打包
                # ============================================
                find_package(Java COMPONENTS Development REQUIRED)
                
                # 设置 JAR 路径变量
                set(DAS_JAVA_JAR_DIR "${DAS_LANGUAGE_BIN_DIR}/Java")
                set(_JAR_OUTPUT_PATH "${DAS_JAVA_JAR_DIR}/${DAS_IDL_EXPORT_NAME}JavaExport.jar")
                
                # 确保 JAR 输出目录存在
                file(MAKE_DIRECTORY "${DAS_JAVA_JAR_DIR}")
                
                # 使用 CMake 脚本在构建时编译打包
                add_custom_command(
                    OUTPUT "${_JAR_OUTPUT_PATH}"
                    COMMAND "${CMAKE_COMMAND}"
                        -DJAVAC_EXECUTABLE="${Java_JAVAC_EXECUTABLE}"
                        -DJAR_EXECUTABLE="${Java_JAR_EXECUTABLE}"
                        -DJAVA_SRC_DIR="${CMAKE_BINARY_DIR}/include/das/Java"
                        -DJAR_OUTPUT_PATH="${_JAR_OUTPUT_PATH}"
                        -P "${CMAKE_SOURCE_DIR}/cmake/DasCompileJavaJar.cmake"
                    DEPENDS ${_EXPORT_LIB_NAME}
                    COMMENT "[das_add_idl_export] Compiling and packaging ${DAS_IDL_EXPORT_NAME}JavaExport.jar"
                    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
                )
                
                add_custom_target(${_EXPORT_LIB_NAME}Jar ALL
                    DEPENDS "${_JAR_OUTPUT_PATH}"
                )
                
                # 设置插件输出目录属性（供 JavaTestPlugin 等引用）
                set_target_properties(${_EXPORT_LIB_NAME}Jar PROPERTIES
                    PLUGIN_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins"
                )
                # 导出 JAR 路径变量
                set(DAS_CORE_JAVA_JAR "${_JAR_OUTPUT_PATH}" PARENT_SCOPE)
                set(DAS_JAVA_JAR_DIR "${DAS_JAVA_JAR_DIR}" PARENT_SCOPE)
                
                message(STATUS "[das_add_idl_export]   JAR output: ${_JAR_OUTPUT_PATH}")
            endif()
        endforeach()
    endif()

    # ====== Lua sol2 导出（非 SWIG 路径） ======
    # Lua 生成现在由 batch pipeline 的 reduce 阶段统一处理。
    # 输出文件通过 --list-outputs 注册到 _DYNAMIC_OUTPUT_LIST，
    # 从中分类到 _ALL_LUA_FILES。此处仅创建 SHARED 库目标。

    if(_HAS_LUA)
        message(STATUS "[das_add_idl_export] Creating Lua sol2 export library (via batch pipeline)")

        include(BuildLua)
        include(FindSol2)

        # Lua 输出文件路径（由 batch gen 的 reduce 阶段生成）
        set(_LUA_CPP_FILE "${_LUA_OUTPUT_DIR}/${DAS_IDL_EXPORT_NAME}_lua_export.cpp")
        set(_LUA_STUB_FILE "${_LUA_OUTPUT_DIR}/${DAS_IDL_EXPORT_NAME}_lua_export.lua")

        # 创建 SHARED 库
        set(_LUA_LIB_NAME "${DAS_IDL_EXPORT_NAME}LuaExport")
        add_library(${_LUA_LIB_NAME} SHARED ${_LUA_CPP_FILE})
        add_dependencies(${_LUA_LIB_NAME} ${_GENERATED_TARGET})
        if(MSVC)
            target_compile_options(${_LUA_LIB_NAME} PRIVATE /bigobj)
        endif()

        target_include_directories(${_LUA_LIB_NAME} PRIVATE
            ${DAS_IDL_EXPORT_OUTPUT_DIR}
            ${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/abi
        )

        target_link_libraries(${_LUA_LIB_NAME} PRIVATE
            sol2::sol2
        )

        # Lua static library (built from source via FindSol2.cmake)
        target_link_libraries(${_LUA_LIB_NAME} PRIVATE Das::Lua)

        # 链接外部依赖库
        if(DAS_IDL_EXPORT_DEPENDS_ON)
            target_link_libraries(${_LUA_LIB_NAME} PRIVATE ${DAS_IDL_EXPORT_DEPENDS_ON})
        endif()

        # 将 .lua 文件复制到输出目录
        add_custom_command(
            TARGET ${_LUA_LIB_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_LUA_STUB_FILE}"
                "$<TARGET_FILE_DIR:${_LUA_LIB_NAME}>/${DAS_IDL_EXPORT_NAME}_lua_export.lua"
            COMMENT "[das_add_idl_export] Copying Lua stub to output directory"
        )

        # 导出 Lua 输出目录变量
        set(${DAS_IDL_EXPORT_NAME}_LUA_OUTPUT_DIR ${_LUA_OUTPUT_DIR} PARENT_SCOPE)
    endif()

    # ====== Node/NAPI 导出（非 SWIG 路径） ======
    # Node 生成由 batch pipeline 的 reduce 阶段统一处理。
    # 此处仅创建可由 Node 加载的 MODULE 目标。

    if(_HAS_NODE)
        message(STATUS "[das_add_idl_export] Creating Node/NAPI export module (via batch pipeline)")

        find_package(NodeApi REQUIRED)

        set(_NODE_CPP_FILE "${_NODE_OUTPUT_DIR}/${DAS_IDL_EXPORT_NODE_ADDON_NAME}_export.cpp")
        set(_NODE_LIB_NAME "${DAS_IDL_EXPORT_NAME}NodeExport")
        get_filename_component(_NODE_GENERATED_INCLUDE_ROOT "${DAS_IDL_EXPORT_OUTPUT_DIR}" DIRECTORY)

        set_source_files_properties("${_NODE_CPP_FILE}" PROPERTIES GENERATED TRUE)

        add_library(${_NODE_LIB_NAME} MODULE "${_NODE_CPP_FILE}")
        add_dependencies(${_NODE_LIB_NAME} ${_GENERATED_TARGET})

        target_include_directories(${_NODE_LIB_NAME} PRIVATE
            "${CMAKE_SOURCE_DIR}/include"
            "${_NODE_GENERATED_INCLUDE_ROOT}"
            "${DAS_IDL_EXPORT_OUTPUT_DIR}"
            "${_ABI_OUTPUT_DIR}"
            "${_HEADER_OUTPUT_DIR}"
        )

        target_link_libraries(${_NODE_LIB_NAME} PRIVATE
            NodeApi::Headers
            NodeApi::NodeAddonApi
            NodeApi::NodeLib
        )

        if(DAS_IDL_EXPORT_DEPENDS_ON)
            target_link_libraries(${_NODE_LIB_NAME} PRIVATE ${DAS_IDL_EXPORT_DEPENDS_ON})
        endif()

        target_compile_definitions(${_NODE_LIB_NAME} PRIVATE NAPI_VERSION=8)

        set_target_properties(${_NODE_LIB_NAME} PROPERTIES
            PREFIX ""
            SUFFIX ".node"
            OUTPUT_NAME "${DAS_IDL_EXPORT_NODE_ADDON_NAME}"
            LIBRARY_OUTPUT_DIRECTORY "${_NODE_OUTPUT_DIR}"
            RUNTIME_OUTPUT_DIRECTORY "${_NODE_OUTPUT_DIR}"
            ARCHIVE_OUTPUT_DIRECTORY "${_NODE_OUTPUT_DIR}"
        )

        foreach(_NODE_CONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
            set_target_properties(${_NODE_LIB_NAME} PROPERTIES
                "LIBRARY_OUTPUT_DIRECTORY_${_NODE_CONFIG}" "${_NODE_OUTPUT_DIR}"
                "RUNTIME_OUTPUT_DIRECTORY_${_NODE_CONFIG}" "${_NODE_OUTPUT_DIR}"
                "ARCHIVE_OUTPUT_DIRECTORY_${_NODE_CONFIG}" "${_NODE_OUTPUT_DIR}"
            )
        endforeach()

        set(${DAS_IDL_EXPORT_NAME}_NODE_OUTPUT_DIR ${_NODE_OUTPUT_DIR} PARENT_SCOPE)
    endif()

    # ====== 构建导出宏列表 ======
    set(_EXPORT_DEFINITIONS "")
    if(DAS_IDL_EXPORT_LANGUAGES)
        foreach(_LANG ${DAS_IDL_EXPORT_LANGUAGES})
            string(TOLOWER "${_LANG}" _LANG_LOWER)
            if(_LANG_LOWER STREQUAL "python")
                list(APPEND _EXPORT_DEFINITIONS "DAS_EXPORT_PYTHON")
            elseif(_LANG_LOWER STREQUAL "java")
                list(APPEND _EXPORT_DEFINITIONS "DAS_EXPORT_JAVA")
            elseif(_LANG_LOWER STREQUAL "csharp")
                list(APPEND _EXPORT_DEFINITIONS "DAS_EXPORT_CSHARP")
            elseif(_LANG_LOWER STREQUAL "lua")
                list(APPEND _EXPORT_DEFINITIONS "DAS_EXPORT_LUA")
            elseif(_LANG_LOWER STREQUAL "node")
                list(APPEND _EXPORT_DEFINITIONS "DAS_EXPORT_NODE")
            endif()
        endforeach()
    endif()

    # ====== 设置输出变量 ======
    if(DAS_IDL_EXPORT_GENERATED_FILES)
        set(${DAS_IDL_EXPORT_GENERATED_FILES} ${_ALL_GENERATED_FILES} PARENT_SCOPE)
    endif()
    if(DAS_IDL_EXPORT_GENERATED_ABI_FILES)
        set(${DAS_IDL_EXPORT_GENERATED_ABI_FILES} ${_ALL_ABI_FILES} PARENT_SCOPE)
    endif()
    if(DAS_IDL_EXPORT_GENERATED_WRAPPER_FILES)
        set(${DAS_IDL_EXPORT_GENERATED_WRAPPER_FILES} ${_ALL_WRAPPER_FILES} PARENT_SCOPE)
    endif()
    if(DAS_IDL_EXPORT_GENERATED_SWIG_FILES)
        set(${DAS_IDL_EXPORT_GENERATED_SWIG_FILES} ${_ALL_SWIG_FILES} PARENT_SCOPE)
    endif()
    if(DAS_IDL_EXPORT_GENERATED_IPC_FILES)
        set(${DAS_IDL_EXPORT_GENERATED_IPC_FILES} ${_ALL_IPC_FILES} PARENT_SCOPE)
    endif()

    # 输出导出宏定义列表
    set(${DAS_IDL_EXPORT_NAME}_EXPORT_DEFINITIONS ${_EXPORT_DEFINITIONS} PARENT_SCOPE)

    # 输出目录变量（供外部使用）
    set(${DAS_IDL_EXPORT_NAME}_ABI_OUTPUT_DIR ${_ABI_OUTPUT_DIR} PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_WRAPPER_OUTPUT_DIR ${_WRAPPER_OUTPUT_DIR} PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_SWIG_OUTPUT_DIR ${_SWIG_OUTPUT_DIR} PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_IPC_OUTPUT_DIR ${_IPC_OUTPUT_DIR} PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_IPC_GENERATED_CPP "${_IPC_OUTPUT_DIR}/IpcGenerated.cpp" PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_IIDS_OUTPUT_DIR ${_IIDS_OUTPUT_DIR} PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_HEADER_OUTPUT_DIR ${_HEADER_OUTPUT_DIR} PARENT_SCOPE)

    # 输出用户指定的 SWIG 文件列表（供外部使用）
    if(DAS_IDL_EXPORT_USER_SWIG_FILES)
        set(${DAS_IDL_EXPORT_NAME}_USER_SWIG_FILES ${DAS_IDL_EXPORT_USER_SWIG_FILES} PARENT_SCOPE)
    endif()

    # 中间文件路径变量（供外部使用）
    set(${DAS_IDL_EXPORT_NAME}_BATCH_CONFIG_FILE ${_BATCH_CONFIG_FILE} PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_UPDATE_LIST_FILE ${_UPDATE_LIST_FILE} PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_GENERATED_STAMP ${_GENERATED_STAMP} PARENT_SCOPE)

    if(_NEED_SWIG)
        set(${DAS_IDL_EXPORT_NAME}_SWIG_DEPS_FILE ${_SWIG_DEPS_FILE} PARENT_SCOPE)
        set(${DAS_IDL_EXPORT_NAME}_SORTED_INTERFACES_FILE ${_SORTED_INTERFACES_FILE} PARENT_SCOPE)
    endif()

    message(STATUS "[das_add_idl_export] IDL export configured for ${DAS_IDL_EXPORT_NAME}")
    message(STATUS "[das_add_idl_export]   Batch config: ${_BATCH_CONFIG_FILE}")
    message(STATUS "[das_add_idl_export]   Update list: ${_UPDATE_LIST_FILE}")
    message(STATUS "[das_add_idl_export]   Targets: ${_UPDATE_LIST_TARGET}, ${_GENERATED_TARGET}, ${_HEADER_GENERATED_TARGET}")
    message(STATUS "[das_add_idl_export]   Header output: ${_HEADER_OUTPUT_DIR}")
    if(_NEED_SWIG)
        message(STATUS "[das_add_idl_export]   SWIG targets: ${_DEPS_EXTRACTED_TARGET}, ${_SORTED_TARGET}")
    endif()
endfunction()
