# ============================================================================
# LTO (Link Time Optimization) 配置
# ============================================================================
# 仅在 Release/RelWithDebInfo 模式下启用 LTO
if(DAS_USE_LTO)
    # 检查构建类型（适用于单配置生成器如 Ninja/Make）
    # 多配置生成器（Visual Studio/Xcode）通过 CMAKE_CONFIGURATION_TYPES 设置
    if(CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        message(WARNING
            "DAS_USE_LTO is enabled but CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}'. "
            "LTO only affects Release/RelWithDebInfo builds. "
            "Consider setting -DCMAKE_BUILD_TYPE=Release")
    endif()

    # 多配置生成器提示
    if(NOT CMAKE_BUILD_TYPE AND CMAKE_CONFIGURATION_TYPES)
        message(STATUS "Multi-config generator detected. LTO will be applied to "
                      "Release and RelWithDebInfo configurations only (Debug/MinSizeRel excluded)")
    endif()

    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_result OUTPUT ipo_error LANGUAGES CXX)

    if(ipo_result)
        message(STATUS "IPO/LTO supported by compiler - enabling for Release/RelWithDebInfo only")

        # 使用生成器表达式确保 LTO 只在 Release/RelWithDebInfo 下启用
        # 显式设置各配置的 IPO 状态，避免多配置生成器污染 Debug 配置
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        # 显式禁用 Debug 的 LTO，确保不会被意外启用
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
        # MinSizeRel 通常也需要禁用 LTO（侧重代码大小而非速度）
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL OFF)

        # MinGW 特殊提示
        if(MINGW)
            message(STATUS "MinGW with LTO: If linker errors occur, ensure "
                          "liblto_plugin-0.dll is in lib/bfd-plugins/")
        endif()

        # 预编译库警告
        if(NOT DAS_USE_BUNDLED_OPENCV)
            message(STATUS "LTO with pre-built OpenCV: If linking fails, "
                          "try DAS_USE_BUNDLED_OPENCV=ON or disable LTO")
        endif()
    else()
        message(WARNING "IPO/LTO not supported by this compiler: ${ipo_error}")
        message(STATUS "Build will continue without LTO")
    endif()
endif()