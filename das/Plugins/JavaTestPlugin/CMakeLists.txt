# das/Plugins/JavaTestPlugin/CMakeLists.txt
# Java 测试插件 CMake 配置
# 仅在 DAS_BUILD_TEST=ON 且 EXPORT_JAVA=ON 时构建

# ============================================
# 构建条件检查
# ============================================

if(NOT DAS_BUILD_TEST)
    message(STATUS "[JavaTestPlugin] DAS_BUILD_TEST is OFF, skipping")
    return()
endif()

if(NOT EXPORT_JAVA)
    message(STATUS "[JavaTestPlugin] EXPORT_JAVA is OFF, skipping")
    return()
endif()

# ============================================
# 依赖检查
# ============================================
# 注意：DAS_CORE_JAVA_JAR 变量由 DasAddIdlExport.cmake 设置
# 在配置阶段可能尚未定义，通过 add_dependencies 处理构建依赖

find_package(Java COMPONENTS Development REQUIRED)

# ============================================
# 插件配置
# ============================================

set(PLUGIN_NAME "JavaTestPlugin")

# 根据生成器类型确定输出路径
# 单配置生成器（Ninja）：配置时确定路径
# 多配置生成器（MSVC）：构建时通过生成器表达式确定路径
if(CMAKE_CONFIGURATION_TYPES)
    # 多配置生成器（MSVC）- 使用生成器表达式
    set(_JAR_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/plugins")
else()
    # 单配置生成器（Ninja）- 配置时确定路径
    set(_JAR_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins")
endif()

set(_OUTPUT_JAR "${_JAR_OUTPUT_DIR}/${PLUGIN_NAME}.jar")
# 收集 Java 源文件
# ============================================

file(GLOB_RECURSE _JAVA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.java")

if(NOT _JAVA_SOURCES)
    message(FATAL_ERROR "[JavaTestPlugin] No Java source files found in ${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

list(LENGTH _JAVA_SOURCES _FILE_COUNT)
message(STATUS "[JavaTestPlugin] Found ${_FILE_COUNT} Java source files")

# ============================================
# 编译打包
# ============================================

# 使用 CMake 脚本在构建时编译打包
add_custom_command(
    OUTPUT "${_OUTPUT_JAR}"
    COMMAND "${CMAKE_COMMAND}"
        -DJAVAC_EXECUTABLE="${Java_JAVAC_EXECUTABLE}"
        -DJAR_EXECUTABLE="${Java_JAR_EXECUTABLE}"
        -DJAVA_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src"
        -DJAR_OUTPUT_PATH="${_OUTPUT_JAR}"
        -DCLASSPATH="${DAS_CORE_JAVA_JAR}"
        -DDEPENDS_JAR="${DAS_CORE_JAVA_JAR}"
        -P "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/DasCompileJavaJar.cmake"
    DEPENDS ${_JAVA_SOURCES}
    COMMENT "[JavaTestPlugin] Building ${PLUGIN_NAME}.jar"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
set(_JAR_TARGET_NAME "${PLUGIN_NAME}Jar")

add_custom_target(${_JAR_TARGET_NAME} ALL
    DEPENDS "${_OUTPUT_JAR}"
)

# 设置插件输出目录属性
set_target_properties(${_JAR_TARGET_NAME} PROPERTIES
    PLUGIN_OUTPUT_DIR "${_JAR_OUTPUT_DIR}"
)


# 确保在 DasCoreJavaExportJar 之后构建
if(TARGET DasCoreJavaExportJar)
    add_dependencies(${_JAR_TARGET_NAME} DasCoreJavaExportJar)
endif()

# 复制插件配置文件到运行时目录
add_custom_command(
    TARGET ${_JAR_TARGET_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_PROPERTY:${_JAR_TARGET_NAME},PLUGIN_OUTPUT_DIR>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.json"
        "$<TARGET_PROPERTY:${_JAR_TARGET_NAME},PLUGIN_OUTPUT_DIR>/${PLUGIN_NAME}.json"
    COMMENT "[JavaTestPlugin] Copying plugin config file"
)

message(STATUS "[JavaTestPlugin] Output: ${_OUTPUT_JAR}")
