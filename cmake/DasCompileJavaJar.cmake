# cmake/DasCompileJavaJar.cmake
# 构建时执行的脚本：收集 Java 文件并编译打包为 JAR
#
# 使用方式:
#   cmake -DJAVAC_EXECUTABLE=<javac路径>
#         -DJAR_EXECUTABLE=<jar工具路径>
#         -DJAVA_SRC_DIR=<Java源文件目录>
#         -DJAR_OUTPUT_PATH=<输出JAR完整路径>
#         [-DCLASSPATH=<依赖classpath>]
#         -P DasCompileJavaJar.cmake

# ============================================
# 参数检查
# ============================================

if(NOT DEFINED JAVAC_EXECUTABLE)
    message(FATAL_ERROR "DasCompileJavaJar: JAVAC_EXECUTABLE is not defined")
endif()

if(NOT DEFINED JAR_EXECUTABLE)
    message(FATAL_ERROR "DasCompileJavaJar: JAR_EXECUTABLE is not defined")
endif()

if(NOT DEFINED JAVA_SRC_DIR)
    message(FATAL_ERROR "DasCompileJavaJar: JAVA_SRC_DIR is not defined")
endif()

if(NOT DEFINED JAR_OUTPUT_PATH)
    message(FATAL_ERROR "DasCompileJavaJar: JAR_OUTPUT_PATH is not defined")
endif()

# ============================================
# 收集 Java 源文件
# ============================================

file(GLOB_RECURSE JAVA_SOURCES "${JAVA_SRC_DIR}/*.java")

if(NOT JAVA_SOURCES)
    message(FATAL_ERROR "DasCompileJavaJar: No Java source files found in ${JAVA_SRC_DIR}")
endif()

list(LENGTH JAVA_SOURCES _FILE_COUNT)
message(STATUS "DasCompileJavaJar: Found ${_FILE_COUNT} Java source files in ${JAVA_SRC_DIR}")

# ============================================
# 准备编译
# ============================================

# 获取 JAR 名称作为临时目录名
get_filename_component(_JAR_NAME "${JAR_OUTPUT_PATH}" NAME_WE)
set(_CLASSES_DIR "${CMAKE_BINARY_DIR}/${_JAR_NAME}_classes")

# 确保输出目录存在
get_filename_component(_JAR_DIR "${JAR_OUTPUT_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${_JAR_DIR}")
file(MAKE_DIRECTORY "${_CLASSES_DIR}")

# ============================================
# 执行编译
# ============================================

set(_JAVAC_ARGS -d "${_CLASSES_DIR}")

# 添加 classpath（如果有）
if(DEFINED CLASSPATH AND NOT CLASSPATH STREQUAL "")
    list(APPEND _JAVAC_ARGS -cp "${CLASSPATH}")
endif()

# 添加源文件
list(APPEND _JAVAC_ARGS ${JAVA_SOURCES})

message(STATUS "DasCompileJavaJar: Compiling Java sources...")
execute_process(
    COMMAND "${JAVAC_EXECUTABLE}" ${_JAVAC_ARGS}
    RESULT_VARIABLE _JAVAC_RESULT
    ERROR_VARIABLE _JAVAC_ERROR
    OUTPUT_VARIABLE _JAVAC_OUTPUT
)

if(NOT _JAVAC_RESULT EQUAL 0)
    message(FATAL_ERROR "DasCompileJavaJar: Java compilation failed:\n${_JAVAC_ERROR}")
endif()

message(STATUS "DasCompileJavaJar: Java compilation successful")

# ============================================
# 打包 JAR
# ============================================

message(STATUS "DasCompileJavaJar: Creating JAR: ${JAR_OUTPUT_PATH}")
execute_process(
    COMMAND "${JAR_EXECUTABLE}" cf "${JAR_OUTPUT_PATH}" -C "${_CLASSES_DIR}" .
    RESULT_VARIABLE _JAR_RESULT
    ERROR_VARIABLE _JAR_ERROR
)

if(NOT _JAR_RESULT EQUAL 0)
    message(FATAL_ERROR "DasCompileJavaJar: JAR creation failed:\n${_JAR_ERROR}")
endif()

message(STATUS "DasCompileJavaJar: JAR created successfully: ${JAR_OUTPUT_PATH}")
