# DasAddIdlExport.cmake
# 通用的 IDL 导出函数，用于生成 ABI、Wrapper、SWIG 接口和 IPC 代码
#
# 功能:
#   - 当 LANGUAGES 为空时：生成 ABI + wrapper + IPC 代码，不执行 find_package(SWIG)
#   - 当 LANGUAGES 非空时：生成 ABI + wrapper + SWIG 接口 + 目标语言绑定 + IPC 代码
#   - IPC 代码默认生成到 ${OUTPUT_DIR}/_autogen/idl/ipc/ 目录
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
#       GENERATE_IPC_MESSAGE OFF
#   )
#
# 生成的目标:
#   - ${NAME}IdlUpdateList: 增量检查目标，生成需要更新的 IDL 列表
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

# 引入依赖模块
if(NOT COMMAND das_idl_generate)
    include("${CMAKE_CURRENT_LIST_DIR}/DasIdlGenerator.cmake")
endif()

# 引入 SWIG 导出库函数（当需要语言绑定时使用）
if(NOT COMMAND das_add_swig_export_library)
    include("${CMAKE_CURRENT_LIST_DIR}/DasUtils.cmake")
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
#   GENERATE_IPC_MESSAGE  - 是否生成 IPC Message 代码，默认 ON
#   NAMESPACE             - 可选的命名空间
#   SWIG_OPTIONS_Python   - Python 的 SWIG 编译选项（可选）
#   SWIG_OPTIONS_Java     - Java 的 SWIG 编译选项（可选）
#   SWIG_OPTIONS_CSharp   - CSharp 的 SWIG 编译选项（可选）
#   SWIG_INCLUDE_DIRS     - 额外的 SWIG 包含目录列表（可选，如 ${CMAKE_SOURCE_DIR}/include/ 等）
#   DEPENDS_ON            - SWIG 导出库依赖的其他目标列表（可选，如 DasCore）
#
# 输出变量 (通过 PARENT_SCOPE 输出):
#   ${NAME}_GENERATED_FILES        - 所有生成的文件列表
#   ${NAME}_GENERATED_ABI_FILES    - ABI 文件列表
#   ${NAME}_GENERATED_WRAPPER_FILES - Wrapper 文件列表
#   ${NAME}_GENERATED_SWIG_FILES   - SWIG 文件列表（代码生成器生成）
#   ${NAME}_USER_SWIG_FILES        - 用户指定的 SWIG 文件列表
function(das_add_idl_export)
    cmake_parse_arguments(
        DAS_IDL_EXPORT                          # 前缀
        ""                                      # 选项 (无值参数)
        "NAME;IDL_DIR;OUTPUT_DIR;NAMESPACE;GENERATE_IPC_PROXY;GENERATE_IPC_STUB;GENERATE_IPC_MESSAGE"  # 单值参数
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
    if(NOT DEFINED DAS_IDL_EXPORT_GENERATE_IPC_MESSAGE)
        set(DAS_IDL_EXPORT_GENERATE_IPC_MESSAGE ON)
    endif()

    # ====== 设置输出目录结构 ======
    # 兼容现有体系: ${OUTPUT_DIR}/_autogen/idl/{abi,wrapper,swig,ipc,iids}
    set(_ABI_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/abi")
    set(_WRAPPER_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/wrapper")
    set(_SWIG_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/swig")
    set(_IPC_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/ipc")
    set(_IIDS_OUTPUT_DIR "${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/iids")

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
    message(STATUS "[das_add_idl_export]   IPC_MESSAGE: ${DAS_IDL_EXPORT_GENERATE_IPC_MESSAGE}")

    # ====== 判断是否需要 SWIG ======
    set(_NEED_SWIG FALSE)
    if(DAS_IDL_EXPORT_LANGUAGES)
        list(LENGTH DAS_IDL_EXPORT_LANGUAGES _LANG_COUNT)
        if(_LANG_COUNT GREATER 0)
            set(_NEED_SWIG TRUE)
        endif()
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
    set(_GENERATED_STAMP "${_ABI_OUTPUT_DIR}/.generated_stamp")

    # ====== 1. 生成批量配置 JSON 文件 ======
    set(_BATCH_JSON_CONFIG "[\n")
    set(_IS_FIRST_IDL TRUE)

    foreach(_IDL_FILE ${_FULL_IDL_PATHS})
        if(_IS_FIRST_IDL)
            set(_IS_FIRST_IDL FALSE)
        else()
            string(APPEND _BATCH_JSON_CONFIG ",\n")
        endif()

        get_filename_component(_IDL_NAME "${_IDL_FILE}" NAME_WE)

        string(APPEND _BATCH_JSON_CONFIG "    {\n")
        string(APPEND _BATCH_JSON_CONFIG "        \"-i\": \"${_IDL_FILE}\",\n")
        string(APPEND _BATCH_JSON_CONFIG "        \"--raw-output-dir\": \"${_ABI_OUTPUT_DIR}\",\n")
        string(APPEND _BATCH_JSON_CONFIG "        \"--wrapper-output-dir\": \"${_WRAPPER_OUTPUT_DIR}\",\n")
        string(APPEND _BATCH_JSON_CONFIG "        \"--implements-output-dir\": \"${_WRAPPER_OUTPUT_DIR}\",\n")

        if(_NEED_SWIG)
            string(APPEND _BATCH_JSON_CONFIG "        \"--swig-output-dir\": \"${_SWIG_OUTPUT_DIR}\",\n")
            string(APPEND _BATCH_JSON_CONFIG "        \"--swig\": true,\n")
        endif()

        string(APPEND _BATCH_JSON_CONFIG "        \"--cpp-wrapper\": true,\n")
        string(APPEND _BATCH_JSON_CONFIG "        \"--cpp-implements\": true,\n")

        # 添加 IPC 选项
        if(DAS_IDL_EXPORT_GENERATE_IPC_PROXY OR DAS_IDL_EXPORT_GENERATE_IPC_STUB OR DAS_IDL_EXPORT_GENERATE_IPC_MESSAGE)
            string(APPEND _BATCH_JSON_CONFIG "        \"--ipc-output-dir\": \"${_IPC_OUTPUT_DIR}\",\n")

            if(DAS_IDL_EXPORT_GENERATE_IPC_PROXY)
                string(APPEND _BATCH_JSON_CONFIG "        \"--ipc-proxy\": true,\n")
            endif()

            if(DAS_IDL_EXPORT_GENERATE_IPC_STUB)
                string(APPEND _BATCH_JSON_CONFIG "        \"--ipc-stub\": true,\n")
            endif()

            if(DAS_IDL_EXPORT_GENERATE_IPC_MESSAGE)
                string(APPEND _BATCH_JSON_CONFIG "        \"--ipc-message\": true,\n")
            endif()
        endif()

        # 添加命名空间
        if(DAS_IDL_EXPORT_NAMESPACE)
            string(APPEND _BATCH_JSON_CONFIG "        \"--namespace\": \"${DAS_IDL_EXPORT_NAMESPACE}\",\n")
        endif()

        string(APPEND _BATCH_JSON_CONFIG "        \"--generate-type-maps\": true\n")
        string(APPEND _BATCH_JSON_CONFIG "    }")
    endforeach()

    string(APPEND _BATCH_JSON_CONFIG "\n]")

    # 将 JSON 配置写入文件
    file(WRITE "${_BATCH_CONFIG_FILE}" "${_BATCH_JSON_CONFIG}")
    message(STATUS "[das_add_idl_export] Generated IDL batch configuration: ${_BATCH_CONFIG_FILE}")

    # ====== 2. 创建增量检查命令和目标 ======
    add_custom_command(
        OUTPUT ${_UPDATE_LIST_FILE}
        COMMAND "${DAS_IDL_VENV_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/das_idl/check_idl_updates.py"
            --config "${_BATCH_CONFIG_FILE}"
            --output "${_UPDATE_LIST_FILE}"
        DEPENDS ${_FULL_IDL_PATHS} "${_BATCH_CONFIG_FILE}"
            ${_DAS_IDL_MODULE_TOOLS}
        COMMENT "[das_add_idl_export] Check which IDL files need to be regenerated for ${DAS_IDL_EXPORT_NAME}"
        VERBATIM
    )

    set(_UPDATE_LIST_TARGET "${DAS_IDL_EXPORT_NAME}IdlUpdateList")
    if(NOT TARGET ${_UPDATE_LIST_TARGET})
        add_custom_target(${_UPDATE_LIST_TARGET}
            DEPENDS ${_UPDATE_LIST_FILE}
        )
    endif()

    # ====== 3. 创建批量生成命令和目标 ======
    add_custom_command(
        OUTPUT ${_GENERATED_STAMP}
        COMMAND "${DAS_IDL_VENV_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/das_idl/das_idl_batch_gen.py"
            --config "${_BATCH_CONFIG_FILE}"
            --update-list "${_UPDATE_LIST_FILE}"
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
            OUTPUT ${_SWIG_DEPS_FILE}
            COMMAND "${DAS_IDL_VENV_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/das_idl/extract_idl_deps.py"
                --idl-dir "${DAS_IDL_EXPORT_IDL_DIR}"
                --output "${_SWIG_DEPS_FILE}"
            DEPENDS ${_FULL_IDL_PATHS}
                "${CMAKE_SOURCE_DIR}/tools/das_idl/extract_idl_deps.py"
            COMMENT "[das_add_idl_export] Extract IDL interface dependencies for ${DAS_IDL_EXPORT_NAME}"
            VERBATIM
        )

        set(_DEPS_EXTRACTED_TARGET "${DAS_IDL_EXPORT_NAME}IdlDepsExtracted")
        if(NOT TARGET ${_DEPS_EXTRACTED_TARGET})
            add_custom_target(${_DEPS_EXTRACTED_TARGET}
                DEPENDS ${_SWIG_DEPS_FILE}
            )
            add_dependencies(${_DEPS_EXTRACTED_TARGET} ${_GENERATED_TARGET})
        endif()

        # 拓扑排序命令
        add_custom_command(
            OUTPUT ${_SORTED_INTERFACES_FILE}
            COMMAND "${DAS_IDL_VENV_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/das_idl/topological_sort.py"
                --deps-file "${_SWIG_DEPS_FILE}"
                --output "${_SORTED_INTERFACES_FILE}"
            DEPENDS ${_SWIG_DEPS_FILE}
                "${CMAKE_SOURCE_DIR}/tools/das_idl/topological_sort.py"
            COMMENT "[das_add_idl_export] Topological sort of SWIG interfaces for ${DAS_IDL_EXPORT_NAME}"
            VERBATIM
        )

        set(_SORTED_TARGET "${DAS_IDL_EXPORT_NAME}SwigInterfacesSorted")
        if(NOT TARGET ${_SORTED_TARGET})
            add_custom_target(${_SORTED_TARGET}
                DEPENDS ${_SORTED_INTERFACES_FILE}
            )
            add_dependencies(${_SORTED_TARGET} ${_DEPS_EXTRACTED_TARGET})
        endif()

        # 设置 swig_all.i 文件路径
        set(_SWIG_ALL_I "${_SWIG_OUTPUT_DIR}/swig_all.i")

        # 生成 swig_all.i 文件（汇总所有 SWIG 接口）
        add_custom_command(
            OUTPUT "${_SWIG_ALL_I}"
            COMMAND ${CMAKE_COMMAND}
                -DSWIG_OUTPUT_DIR=${_SWIG_OUTPUT_DIR}
                -DABI_OUTPUT_DIR=${_ABI_OUTPUT_DIR}
                -DSORTED_INTERFACES_FILE=${_SORTED_INTERFACES_FILE}
                -P ${CMAKE_SOURCE_DIR}/cmake/generate_swig_all_i.cmake
            COMMAND ${CMAKE_COMMAND} -E touch ${_SWIG_OUTPUT_DIR}/.swig_all_generated
            DEPENDS
                ${_SORTED_INTERFACES_FILE}
                ${CMAKE_SOURCE_DIR}/cmake/generate_swig_all_i.cmake
            BYPRODUCTS ${_SWIG_OUTPUT_DIR}/.swig_all_generated
            COMMENT "Generate SWIG swig_all.i summary file for ${DAS_IDL_EXPORT_NAME}"
            VERBATIM
        )

        # 创建 swig_all.i 生成目标
        set(_SWIG_ALL_TARGET "${DAS_IDL_EXPORT_NAME}SwigAllGenerated")
        if(NOT TARGET ${_SWIG_ALL_TARGET})
            add_custom_target(${_SWIG_ALL_TARGET}
                DEPENDS "${_SWIG_ALL_I}"
            )
            add_dependencies(${_SWIG_ALL_TARGET} ${_SORTED_TARGET})
        endif()
    endif()

    # ====== 收集所有生成的文件 (用于输出变量和兼容性) ======
    set(_ALL_ABI_FILES "")
    set(_ALL_WRAPPER_FILES "")
    set(_ALL_SWIG_FILES "")
    set(_ALL_IPC_FILES "")
    set(_ALL_GENERATED_FILES "")

    # 为每个 IDL 文件构建预期的生成文件列表
    foreach(_IDL_FILE_NAME ${DAS_IDL_EXPORT_IDL_FILES})
        get_filename_component(_IDL_NAME "${_IDL_FILE_NAME}" NAME_WE)

        # ABI 文件
        list(APPEND _ALL_ABI_FILES
            "${_ABI_OUTPUT_DIR}/${_IDL_NAME}.h"
            "${_ABI_OUTPUT_DIR}/${_IDL_NAME}_Swig.h"
        )

        # Wrapper 文件
        list(APPEND _ALL_WRAPPER_FILES
            "${_WRAPPER_OUTPUT_DIR}/${_IDL_NAME}_wrapper.h"
            "${_WRAPPER_OUTPUT_DIR}/${_IDL_NAME}_wrapper.cpp"
        )

        # SWIG 文件 (如果需要)
        if(_NEED_SWIG)
            list(APPEND _ALL_SWIG_FILES
                "${_SWIG_OUTPUT_DIR}/${_IDL_NAME}_swig.i"
            )
        endif()

        # IPC 文件 (如果需要)
        if(DAS_IDL_EXPORT_GENERATE_IPC_MESSAGE)
            list(APPEND _ALL_IPC_FILES
                "${_IPC_OUTPUT_DIR}/messages/${_IDL_NAME}Messages.h"
            )
        endif()
        if(DAS_IDL_EXPORT_GENERATE_IPC_PROXY)
            list(APPEND _ALL_IPC_FILES
                "${_IPC_OUTPUT_DIR}/proxy/${_IDL_NAME}Proxy.h"
            )
        endif()
        if(DAS_IDL_EXPORT_GENERATE_IPC_STUB)
            list(APPEND _ALL_IPC_FILES
                "${_IPC_OUTPUT_DIR}/stub/${_IDL_NAME}Stub.h"
            )
        endif()
    endforeach()

    # 合并所有生成的文件
    set(_ALL_GENERATED_FILES ${_ALL_ABI_FILES})
    if(_ALL_WRAPPER_FILES)
        list(APPEND _ALL_GENERATED_FILES ${_ALL_WRAPPER_FILES})
    endif()
    if(_ALL_SWIG_FILES)
        list(APPEND _ALL_GENERATED_FILES ${_ALL_SWIG_FILES})
    endif()
    if(_ALL_IPC_FILES)
        list(APPEND _ALL_GENERATED_FILES ${_ALL_IPC_FILES})
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
                find_package(Python3 COMPONENTS Development REQUIRED)
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

            das_add_swig_export_library(
                ${_LANG_NAME}
                ${_EXPORT_LIB_NAME}
                "${_SWIG_FILES_TO_USE}"
            )

            # 设置 SWIG 包含目录
            set(_SWIG_INCLUDE_DIRS
                ${DAS_IDL_EXPORT_OUTPUT_DIR}
                ${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/abi
                ${DAS_IDL_EXPORT_OUTPUT_DIR}/_autogen/idl/swig)

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
                target_link_libraries(${_EXPORT_LIB_NAME} PRIVATE Python3::Python)
            elseif(_LANG_NAME STREQUAL "Java")
                target_link_libraries(${_EXPORT_LIB_NAME} PRIVATE JNI::JNI)
            endif()
            # CSharp 不需要链接额外的虚拟机库
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

    # 输出目录变量（供外部使用）
    set(${DAS_IDL_EXPORT_NAME}_ABI_OUTPUT_DIR ${_ABI_OUTPUT_DIR} PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_WRAPPER_OUTPUT_DIR ${_WRAPPER_OUTPUT_DIR} PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_SWIG_OUTPUT_DIR ${_SWIG_OUTPUT_DIR} PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_IPC_OUTPUT_DIR ${_IPC_OUTPUT_DIR} PARENT_SCOPE)
    set(${DAS_IDL_EXPORT_NAME}_IIDS_OUTPUT_DIR ${_IIDS_OUTPUT_DIR} PARENT_SCOPE)

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
    message(STATUS "[das_add_idl_export]   Targets: ${_UPDATE_LIST_TARGET}, ${_GENERATED_TARGET}")
    if(_NEED_SWIG)
        message(STATUS "[das_add_idl_export]   SWIG targets: ${_DEPS_EXTRACTED_TARGET}, ${_SORTED_TARGET}")
    endif()
endfunction()
