# DasPythonFallback.cmake
#
# 处理 MSVC 下 Python pyconfig.h 自动链接与 Python3::SABIModule 的冲突。
#
# 问题背景：
#   项目使用 Python3::SABIModule（Stable ABI）链接 Python，它提供 python3.lib /
#   python3_d.lib（全路径，由 CMake 管理）。但 Python 的 pyconfig.h 会通过
#   MSVC 专有的 #pragma comment(lib) 额外注入 python313.lib / python313_d.lib
#   （不带路径的版本特定库名）。
#
#   链接器搜索路径中通常不包含 Python 的 libs/ 目录，导致 LNK1104。
#   即使 python313_d.lib 文件存在于磁盘上，仅凭 #pragma comment(lib) 的裸文件名
#   链接器也无法找到它。
#
#   GCC/Clang 不支持 #pragma comment(lib)，因此不受影响。
#
# 此模块提供以下功能：
#   das_python_debug_fallback() — 检测环境，设置全局状态，创建包装目标
#
# 创建的 CMake 目标：
#   DasPython::SABI — 包装 Python3::SABIModule 的 INTERFACE IMPORTED 库，
#                     在 MSVC Debug 配置下自动携带 /NODEFAULTLIB:pythonXXX_d.lib
#
# 用法：
#   1. 在 find_package(Python3) 后调用 das_python_debug_fallback()
#   2. 所有需要链接 Python 的地方使用 DasPython::SABI 代替 Python3::SABIModule

# 检测 Python debug 库可用性，设置全局变量，创建包装目标。
#
# 前置条件：已调用 find_package(Python3 COMPONENTS Development [...])
#
# 设置以下变量：
#   DAS_PYTHON_DEBUG_AVAILABLE — BOOL: Python debug 库是否可用
#
# 创建以下目标：
#   DasPython::SABI — INTERFACE IMPORTED 库，链接 Python3::SABIModule
#
function(das_python_debug_fallback)
    if(NOT Python3_FOUND)
        return()
    endif()

    if(Python3_LIBRARY_DEBUG)
        set(DAS_PYTHON_DEBUG_AVAILABLE ON PARENT_SCOPE)
    else()
        set(DAS_PYTHON_DEBUG_AVAILABLE OFF PARENT_SCOPE)

        # debug 库不可用时，用 Release 库覆盖 CMake 导入目标的 Debug 属性
        # 这样 $<CONFIG:Debug> 也会链接 Release Python 库
        foreach(_TGT Python3::Python Python3::Module Python3::SABIModule)
            if(TARGET ${_TGT})
                get_target_property(_REL_IMPLIB ${_TGT} IMPORTED_IMPLIB_RELEASE)
                get_target_property(_REL_LOCATION ${_TGT} IMPORTED_LOCATION_RELEASE)

                if(_REL_IMPLIB)
                    set_target_properties(${_TGT} PROPERTIES
                        IMPORTED_IMPLIB_DEBUG "${_REL_IMPLIB}"
                    )
                endif()
                if(_REL_LOCATION)
                    set_target_properties(${_TGT} PROPERTIES
                        IMPORTED_LOCATION_DEBUG "${_REL_LOCATION}"
                    )
                endif()
            endif()
        endforeach()

        message(STATUS "[Python Fallback] Python debug library not available, using release for all configs.")
    endif()

    # 创建 DasPython::SABI 包装目标
    # 不能直接修改 Python3::SABIModule（IMPORTED 目标属性不可更改），
    # 所以创建一个新的 INTERFACE IMPORTED 库来包装它。
    if(TARGET Python3::SABIModule AND NOT TARGET DasPython::SABI)
        add_library(DasPython::SABI INTERFACE IMPORTED)
        set_target_properties(DasPython::SABI PROPERTIES
            INTERFACE_LINK_LIBRARIES "Python3::SABIModule"
        )

        # MSVC：阻止 pyconfig.h 注入的版本特定 debug 库。
        # /NODEFAULTLIB:pythonXXX_d.lib 只阻止这一个库，不影响其他链接。
        if(MSVC)
            set(_VER_DEBUG_LIB "python${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}_d.lib")

            set_property(TARGET DasPython::SABI APPEND PROPERTY
                INTERFACE_LINK_OPTIONS "$<$<CONFIG:Debug>:/NODEFAULTLIB:${_VER_DEBUG_LIB}>"
            )

            message(STATUS "[Python Fallback] DasPython::SABI created, /NODEFAULTLIB:${_VER_DEBUG_LIB} on Debug")
        else()
            message(STATUS "[Python Fallback] DasPython::SABI created (no /NODEFAULTLIB needed on non-MSVC)")
        endif()
    endif()
endfunction()
