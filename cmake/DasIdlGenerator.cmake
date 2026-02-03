# DasIdlGenerator.cmake
# 用于配置 Python 虚拟环境并运行 IDL 代码生成器
# 不污染用户本地 Python 环境

# 查找 Python 解释器
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# 定义虚拟环境路径
set(DAS_IDL_VENV_DIR "${CMAKE_BINARY_DIR}/_das_idl_venv")
set(DAS_IDL_TOOLS_DIR "${CMAKE_SOURCE_DIR}/tools/das_idl")
set(DAS_IDL_REQUIREMENTS "${DAS_IDL_TOOLS_DIR}/requirements.txt")

# 根据平台设置虚拟环境中的 Python 可执行文件路径
if(WIN32)
    set(DAS_IDL_VENV_PYTHON "${DAS_IDL_VENV_DIR}/Scripts/python.exe")
    set(DAS_IDL_VENV_PIP "${DAS_IDL_VENV_DIR}/Scripts/pip.exe")
    # 某些环境下，Windows的venv可能使用bin目录而不是Scripts目录
    if(NOT EXISTS "${DAS_IDL_VENV_PYTHON}")
        set(DAS_IDL_VENV_PYTHON "${DAS_IDL_VENV_DIR}/bin/python.exe")
        set(DAS_IDL_VENV_PIP "${DAS_IDL_VENV_DIR}/bin/pip.exe")
    endif()
else()
    set(DAS_IDL_VENV_PYTHON "${DAS_IDL_VENV_DIR}/bin/python")
    set(DAS_IDL_VENV_PIP "${DAS_IDL_VENV_DIR}/bin/pip")
endif()

# 创建虚拟环境的函数
function(das_idl_setup_venv)
    # 检查虚拟环境是否已存在
    if(NOT EXISTS "${DAS_IDL_VENV_PYTHON}")
        message(STATUS "[DAS IDL] Creating Python virtual environment at ${DAS_IDL_VENV_DIR}")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -m venv "${DAS_IDL_VENV_DIR}"
            RESULT_VARIABLE VENV_RESULT
            OUTPUT_VARIABLE VENV_OUTPUT
            ERROR_VARIABLE VENV_ERROR
            TIMEOUT 120
        )
        if(NOT VENV_RESULT EQUAL 0)
            message(FATAL_ERROR "[DAS IDL] Failed to create virtual environment: ${VENV_ERROR}")
        endif()
    else()
        message(STATUS "[DAS IDL] Using existing virtual environment at ${DAS_IDL_VENV_DIR}")
    endif()

    # 安装依赖
    if(EXISTS "${DAS_IDL_REQUIREMENTS}")
        message(STATUS "[DAS IDL] Installing Python dependencies from ${DAS_IDL_REQUIREMENTS}")
        execute_process(
            COMMAND "${DAS_IDL_VENV_PIP}" install -r "${DAS_IDL_REQUIREMENTS}" -q
            RESULT_VARIABLE PIP_RESULT
            OUTPUT_VARIABLE PIP_OUTPUT
            ERROR_VARIABLE PIP_ERROR
        )
        if(NOT PIP_RESULT EQUAL 0)
            # 检查是否是因为requirements.txt为空而导致的错误
            file(READ "${DAS_IDL_REQUIREMENTS}" REQUIREMENTS_CONTENT)
            string(STRIP "${REQUIREMENTS_CONTENT}" REQUIREMENTS_CONTENT)

            # 如果requirements.txt为空或只有注释，这是正常情况，不报错
            if(REQUIREMENTS_CONTENT STREQUAL "" OR REQUIREMENTS_CONTENT MATCHES "^[#\\s]*$")
                message(STATUS "[DAS IDL] No dependencies to install (requirements.txt is empty or contains only comments)")
            else()
                message(WARNING "[DAS IDL] Failed to install dependencies: ${PIP_ERROR}")
            endif()
        endif()
    else()
        message(STATUS "[DAS IDL] No requirements.txt found, skipping dependency installation")
    endif()
endfunction()

# 定义生成 IDL 的宏
# 参数:
#   IDL_FILE - IDL 输入文件路径
#   OUTPUT_DIR - 生成代码的输出目录（默认参数，向后兼容）
#   NAMESPACE - 命名空间
#   SWIG - 是否生成SWIG绑定（可选，默认false）
#   CPP_WRAPPER - 是否生成C++包装代码（可选，默认false）
#   RAW_OUTPUT_DIR - ABI文件输出目录（可选，优先级高于OUTPUT_DIR）
#   WRAPPER_OUTPUT_DIR - C++包装文件输出目录（可选）
#   SWIG_OUTPUT_DIR - SWIG接口文件输出目录（可选）
#   GENERATED_FILES - 输出变量，包含生成的文件列表（兼容旧参数）
#   GENERATED_ABI_FILES - 输出变量，包含ABI文件列表
#   GENERATED_WRAPPER_FILES - 输出变量，包含Wrapper文件列表
#   GENERATED_SWIG_FILES - 输出变量，包含SWIG文件列表
function(das_idl_generate)
    cmake_parse_arguments(
        DAS_IDL                         # 前缀
        "SWIG;CPP_WRAPPER;TYPEMAPS"     # 选项 (无值参数)
        "IDL_FILE;OUTPUT_DIR;NAMESPACE;RAW_OUTPUT_DIR;WRAPPER_OUTPUT_DIR;SWIG_OUTPUT_DIR" # 单值参数
        "GENERATED_FILES;GENERATED_ABI_FILES;GENERATED_WRAPPER_FILES;GENERATED_SWIG_FILES" # 多值参数
        ${ARGN}
    )

    if(NOT DAS_IDL_IDL_FILE)
        message(FATAL_ERROR "[DAS IDL] IDL_FILE is required")
    endif()

    # 确定输出目录：优先使用新参数，否则使用OUTPUT_DIR作为默认
    if(DAS_IDL_RAW_OUTPUT_DIR)
        set(DAS_IDL_ABI_DIR "${DAS_IDL_RAW_OUTPUT_DIR}")
    elseif(DAS_IDL_OUTPUT_DIR)
        set(DAS_IDL_ABI_DIR "${DAS_IDL_OUTPUT_DIR}")
    else()
        set(DAS_IDL_ABI_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    endif()

    # Wrapper和SWIG目录默认值
    if(NOT DAS_IDL_WRAPPER_OUTPUT_DIR AND DAS_IDL_CPP_WRAPPER)
        set(DAS_IDL_WRAPPER_OUTPUT_DIR "${DAS_IDL_ABI_DIR}/wrapper")
    endif()
    if(NOT DAS_IDL_SWIG_OUTPUT_DIR AND DAS_IDL_SWIG)
        set(DAS_IDL_SWIG_OUTPUT_DIR "${DAS_IDL_ABI_DIR}/swig")
    endif()

    if(NOT DAS_IDL_OUTPUT_DIR)
        set(DAS_IDL_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    endif()

    # 确保虚拟环境已设置
    das_idl_setup_venv()

    # 获取 IDL 文件名（不含扩展名）作为生成文件的基础名称
    get_filename_component(IDL_NAME "${DAS_IDL_IDL_FILE}" NAME_WE)

    # 定义生成的文件列表 - ABI文件
    set(_GENERATED_ABI_FILES
        "${DAS_IDL_ABI_DIR}/${IDL_NAME}.h"
        "${DAS_IDL_ABI_DIR}/${IDL_NAME}_Swig.h"
    )

    # 定义生成的文件列表 - Wrapper文件
    set(_GENERATED_WRAPPER_FILES "")
    if(DAS_IDL_CPP_WRAPPER AND DAS_IDL_WRAPPER_OUTPUT_DIR)
        set(_GENERATED_WRAPPER_FILES
            "${DAS_IDL_WRAPPER_OUTPUT_DIR}/${IDL_NAME}_wrapper.h"
            "${DAS_IDL_WRAPPER_OUTPUT_DIR}/${IDL_NAME}_wrapper.cpp"
        )
    endif()

    # 定义生成的文件列表 - SWIG文件
    set(_GENERATED_SWIG_FILES "")
    if(DAS_IDL_SWIG AND DAS_IDL_SWIG_OUTPUT_DIR)
        set(_GENERATED_SWIG_FILES
            "${DAS_IDL_SWIG_OUTPUT_DIR}/${IDL_NAME}_swig.i"
        )
    endif()

    # 合并所有生成的文件（向后兼容）
    set(_GENERATED_FILES ${_GENERATED_ABI_FILES})
    if(_GENERATED_WRAPPER_FILES)
        list(APPEND _GENERATED_FILES ${_GENERATED_WRAPPER_FILES})
    endif()
    if(_GENERATED_SWIG_FILES)
        list(APPEND _GENERATED_FILES ${_GENERATED_SWIG_FILES})
    endif()

    # 构建命令参数
    set(_IDL_CMD_ARGS
        "${DAS_IDL_TOOLS_DIR}/das_idl_gen.py"
        "--input" "${DAS_IDL_IDL_FILE}"
    )

    # 添加输出目录参数
    list(APPEND _IDL_CMD_ARGS "--raw-output-dir" "${DAS_IDL_ABI_DIR}")

    if(DAS_IDL_CPP_WRAPPER AND DAS_IDL_WRAPPER_OUTPUT_DIR)
        list(APPEND _IDL_CMD_ARGS "--wrapper-output-dir" "${DAS_IDL_WRAPPER_OUTPUT_DIR}")
    endif()

    if(DAS_IDL_SWIG AND DAS_IDL_SWIG_OUTPUT_DIR)
        list(APPEND _IDL_CMD_ARGS "--swig-output-dir" "${DAS_IDL_SWIG_OUTPUT_DIR}")
    endif()

    # 添加布尔选项参数
    if(DAS_IDL_SWIG)
        list(APPEND _IDL_CMD_ARGS "--swig")
    endif()

    if(DAS_IDL_CPP_WRAPPER)
        list(APPEND _IDL_CMD_ARGS "--cpp-wrapper")
    endif()

    if(DAS_IDL_TYPEMAPS)
        list(APPEND _IDL_CMD_ARGS "--generate-type-maps")
    endif()

    if(DAS_IDL_NAMESPACE)
        list(APPEND _IDL_CMD_ARGS "--namespace" "${DAS_IDL_NAMESPACE}")
    endif()

    # 添加自定义命令
    add_custom_command(
        OUTPUT ${_GENERATED_FILES}
        COMMAND "${DAS_IDL_VENV_PYTHON}" ${_IDL_CMD_ARGS}
        DEPENDS "${DAS_IDL_IDL_FILE}"
                "${DAS_IDL_TOOLS_DIR}/das_idl_gen.py"
                "${DAS_IDL_TOOLS_DIR}/das_idl_parser.py"
                "${DAS_IDL_TOOLS_DIR}/das_cpp_generator.py"
                "${DAS_IDL_TOOLS_DIR}/das_cpp_wrapper_generator.py"
                "${DAS_IDL_TOOLS_DIR}/das_swig_generator.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "[DAS IDL] Generating code from ${IDL_NAME}.idl"
        VERBATIM
    )

    # 设置输出变量
    if(DAS_IDL_GENERATED_FILES)
        set(${DAS_IDL_GENERATED_FILES} ${_GENERATED_FILES} PARENT_SCOPE)
    endif()
    if(DAS_IDL_GENERATED_ABI_FILES)
        set(${DAS_IDL_GENERATED_ABI_FILES} ${_GENERATED_ABI_FILES} PARENT_SCOPE)
    endif()
    if(DAS_IDL_GENERATED_WRAPPER_FILES)
        set(${DAS_IDL_GENERATED_WRAPPER_FILES} ${_GENERATED_WRAPPER_FILES} PARENT_SCOPE)
    endif()
    if(DAS_IDL_GENERATED_SWIG_FILES)
        set(${DAS_IDL_GENERATED_SWIG_FILES} ${_GENERATED_SWIG_FILES} PARENT_SCOPE)
    endif()
endfunction()

# 批量处理多个 IDL 文件
# 参数:
#   IDL_DIR - IDL 文件所在目录
#   OUTPUT_DIR - 生成代码的输出目录
#   GENERATED_FILES - 输出变量，包含所有生成的文件列表
function(das_idl_generate_all)
    cmake_parse_arguments(
        DAS_IDL
        ""
        "IDL_DIR;OUTPUT_DIR;NAMESPACE;TYPEMAPS"
        "GENERATED_FILES"
        ${ARGN}
    )

    if(NOT DAS_IDL_IDL_DIR)
        message(FATAL_ERROR "[DAS IDL] IDL_DIR is required")
    endif()

    if(NOT DAS_IDL_OUTPUT_DIR)
        set(DAS_IDL_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    endif()

    # 查找所有 IDL 文件
    file(GLOB IDL_FILES "${DAS_IDL_IDL_DIR}/*.idl")

    set(_ALL_GENERATED_FILES)

    foreach(IDL_FILE ${IDL_FILES})
        if(DAS_IDL_TYPEMAPS)
            das_idl_generate(
                IDL_FILE "${IDL_FILE}"
                OUTPUT_DIR "${DAS_IDL_OUTPUT_DIR}"
                NAMESPACE "${DAS_IDL_NAMESPACE}"
                GENERATED_FILES _SINGLE_GENERATED
                TYPEMAPS
            )
        else()
            das_idl_generate(
                IDL_FILE "${IDL_FILE}"
                OUTPUT_DIR "${DAS_IDL_OUTPUT_DIR}"
                NAMESPACE "${DAS_IDL_NAMESPACE}"
                GENERATED_FILES _SINGLE_GENERATED
            )
        endif()
        list(APPEND _ALL_GENERATED_FILES ${_SINGLE_GENERATED})
    endforeach()

    # 设置输出变量
    if(DAS_IDL_GENERATED_FILES)
        set(${DAS_IDL_GENERATED_FILES} ${_ALL_GENERATED_FILES} PARENT_SCOPE)
    endif()
endfunction()

# 创建一个用于生成所有 IDL 的目标
function(das_idl_add_generate_target TARGET_NAME)
    cmake_parse_arguments(
        DAS_IDL
        ""
        "IDL_DIR;OUTPUT_DIR;NAMESPACE;TYPEMAPS"
        ""
        ${ARGN}
    )

    if(DAS_IDL_TYPEMAPS)
        das_idl_generate_all(
            IDL_DIR "${DAS_IDL_IDL_DIR}"
            OUTPUT_DIR "${DAS_IDL_OUTPUT_DIR}"
            NAMESPACE "${DAS_IDL_NAMESPACE}"
            GENERATED_FILES _GENERATED_FILES
            TYPEMAPS
        )
    else()
        das_idl_generate_all(
            IDL_DIR "${DAS_IDL_IDL_DIR}"
            OUTPUT_DIR "${DAS_IDL_OUTPUT_DIR}"
            NAMESPACE "${DAS_IDL_NAMESPACE}"
            GENERATED_FILES _GENERATED_FILES
        )
    endif()

    add_custom_target(${TARGET_NAME}
        DEPENDS ${_GENERATED_FILES}
        COMMENT "[DAS IDL] Building target ${TARGET_NAME}"
    )
endfunction()
