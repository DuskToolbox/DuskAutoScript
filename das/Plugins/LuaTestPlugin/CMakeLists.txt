# das/Plugins/LuaTestPlugin/CMakeLists.txt
# Lua 测试插件 CMake 配置
# 仅在 DAS_BUILD_TEST=ON 且 EXPORT_LUA=ON 时构建

# ============================================
# 构建条件检查
# ============================================

if(NOT DAS_BUILD_TEST)
    message(STATUS "[LuaTestPlugin] DAS_BUILD_TEST is OFF, skipping")
    return()
endif()

if(NOT EXPORT_LUA)
    message(STATUS "[LuaTestPlugin] EXPORT_LUA is OFF, skipping")
    return()
endif()

# ============================================
# 插件配置
# ============================================

set(PLUGIN_NAME "LuaTestPlugin")

# 根据生成器类型确定输出路径
if(CMAKE_CONFIGURATION_TYPES)
    set(_LUA_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/plugins")
else()
    set(_LUA_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins")
endif()

# ============================================
# 复制插件文件到输出目录
# ============================================

add_custom_target(${PLUGIN_NAME}Copy ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_LUA_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.lua"
        "${_LUA_OUTPUT_DIR}/${PLUGIN_NAME}.lua"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}.json"
        "${_LUA_OUTPUT_DIR}/${PLUGIN_NAME}.json"
    COMMENT "[LuaTestPlugin] Copying plugin files to output directory"
)

message(STATUS "[LuaTestPlugin] Output: ${_LUA_OUTPUT_DIR}")
