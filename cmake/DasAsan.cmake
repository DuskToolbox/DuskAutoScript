# DasAsan.cmake - AddressSanitizer configuration

# ============================================================================
# AddressSanitizer (ASAN) 配置
# 注意：ASAN 仅在 Debug 构建类型下生效
# ============================================================================

if(DAS_USE_ASAN)
    # ASAN 仅在 Debug 构建下生效
    # 单配置生成器（Ninja, Makefile 等）：CMAKE_BUILD_TYPE 在配置时确定
    # 多配置生成器（Visual Studio, Xcode 等）：需要生成器表达式在构建时选择
    if(NOT CMAKE_BUILD_TYPE AND CMAKE_CONFIGURATION_TYPES)
        # 多配置生成器：使用生成器表达式，构建时按配置选择
        message(STATUS "Multi-config generator detected. ASAN will be enabled for Debug configuration.")
        if(MSVC)
            add_compile_options($<$<STREQUAL:$<CONFIG>,Debug>:/fsanitize=address>)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            add_compile_options($<$<STREQUAL:$<CONFIG>,Debug>:-fsanitize=address>)
            add_compile_options($<$<STREQUAL:$<CONFIG>,Debug>:-fno-omit-frame-pointer>)
            add_link_options($<$<STREQUAL:$<CONFIG>,Debug>:-fsanitize=address>)
        endif()
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        # 单配置生成器：直接检查 CMAKE_BUILD_TYPE
        if(MSVC)
            add_compile_options(/fsanitize=address)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
            add_link_options(-fsanitize=address)
        endif()
    else()
        message(STATUS "DAS_USE_ASAN is ON but build type is '${CMAKE_BUILD_TYPE}', ASAN is only enabled for Debug builds.")
        return()
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

    # 非 Debug 构建不启用 ASAN
    if(CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        return()
    endif()

    foreach(target ${ARGV})
        if(TARGET ${target})
            if(MSVC)
                target_compile_options(${target} PRIVATE /fsanitize=address)
            elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
                target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
                target_link_options(${target} PRIVATE -fsanitize=address)
            endif()
            message(STATUS "ASAN enabled for target: ${target}")
        endif()
    endforeach()
endfunction()
