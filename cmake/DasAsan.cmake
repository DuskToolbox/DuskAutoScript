# DasAsan.cmake - AddressSanitizer configuration

# ============================================================================
# AddressSanitizer (ASAN) 配置
# 注意：ASAN 仅在 Debug 构建类型下生效
# ============================================================================

if(DAS_USE_ASAN)
    # 多配置生成器检测
    if(NOT CMAKE_BUILD_TYPE AND CMAKE_CONFIGURATION_TYPES)
        message(STATUS "Multi-config generator detected. ASAN will be enabled for Debug configuration.")
    endif()
    
    # 使用生成器表达式：只有 Debug 配置才会添加 ASAN 选项
    if(MSVC)
        # MSVC: 只添加编译器选项，链接器会自动处理
        add_compile_options($<$<STREQUAL:$<CONFIG>,Debug>:/fsanitize=address>)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options($<$<STREQUAL:$<CONFIG>,Debug>:-fsanitize=address $<$<STREQUAL:$<CONFIG>,Debug>:-fno-omit-frame-pointer>)
        add_link_options($<$<STREQUAL:$<CONFIG>,Debug>:-fsanitize=address>)
    endif()
    
    message(STATUS "AddressSanitizer enabled for Debug builds")
endif()

# ============================================================================
# 函数：为一组 targets 启用 ASAN（用于第三方库）
# ============================================================================
function(das_enable_asan_for_targets)
    if(NOT DAS_USE_ASAN)
        return()
    endif()
    
    foreach(target ${ARGV})
        if(TARGET ${target})
            if(MSVC)
                target_compile_options(${target} PRIVATE $<$<STREQUAL:$<CONFIG>,Debug>:/fsanitize=address>)
            elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
                target_compile_options(${target} PRIVATE $<$<STREQUAL:$<CONFIG>,Debug>:-fsanitize=address -fno-omit-frame-pointer>)
                target_link_options(${target} PRIVATE $<$<STREQUAL:$<CONFIG>,Debug>:-fsanitize=address>)
            endif()
            message(STATUS "ASAN enabled for target: ${target}")
        endif()
    endforeach()
endfunction()
