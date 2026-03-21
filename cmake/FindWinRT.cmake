# cmake/FindWinRT.cmake
# 查找 Windows SDK 中的 C++/WinRT 头文件
#
# 使用方式:
#   find_package(WinRT REQUIRED)
#   或者手动指定路径:
#   set(WINRT_SDK_ROOT "C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0" CACHE PATH "")
#   find_package(WinRT REQUIRED)
#
# 提供的变量:
#   WinRT_FOUND         - 是否找到
#   WinRT_INCLUDE_DIR  - C++/WinRT 头文件目录 (cppwinrt 子目录)
#   WINRT_SDK_ROOT     - Windows SDK Include 根目录

include(FindPackageHandleStandardArgs)

set(WinRT_FOUND FALSE)
set(WinRT_INCLUDE_DIR "")

# 搜索路径列表
set(_search_paths "")

# 用户直接指定的路径
if(WINRT_SDK_ROOT)
    list(APPEND _search_paths "${WINRT_SDK_ROOT}")
endif()

# 环境变量 WindowsSdkDir
if(DEFINED ENV{WindowsSdkDir})
    file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" _sdk_dir)
    if(EXISTS "${_sdk_dir}")
        list(APPEND _search_paths "${_sdk_dir}")
    endif()
endif()

# 环境变量 WindowsSDK_DIR
if(DEFINED ENV{WindowsSDK_DIR})
    file(TO_CMAKE_PATH "$ENV{WindowsSDK_DIR}" _sdk_dir)
    if(EXISTS "${_sdk_dir}")
        list(APPEND _search_paths "${_sdk_dir}")
    endif()
endif()

# 常见安装目录
list(APPEND _search_paths "C:/Program Files (x86)/Windows Kits/10/Include")

# 去重
if(_search_paths)
    list(REMOVE_DUPLICATES _search_paths)
endif()

# 自动检测：从每个路径查找 cppwinrt 子目录
foreach(_base IN LISTS _search_paths)
    if(WinRT_FOUND)
        break()
    endif()
    
    # 检查基础目录是否存在
    if(NOT EXISTS "${_base}")
        continue()
    endif()
    
    # 查找所有版本目录 (10.0.xxxxx.0)
    file(GLOB _versions "${_base}/10.0.*")
    if(_versions)
        # 按版本号排序，取最新版本
        list(SORT _versions)
        list(REVERSE _versions)
    endif()
    
    # 也检查 base 目录本身（用户可能直接指定了完整路径）
    list(PREPEND _versions "${_base}")
    
    foreach(_version_dir IN LISTS _versions)
        if(WinRT_FOUND)
            break()
        endif()
        
        # 检查 cppwinrt 子目录
        set(_cppwinrt_dir "${_version_dir}/cppwinrt")
        if(EXISTS "${_cppwinrt_dir}/winrt")
            # 验证 winrt 子目录存在
            set(WinRT_SDK_ROOT "${_version_dir}" CACHE PATH "Windows SDK Include root directory" FORCE)
            set(WinRT_INCLUDE_DIR "${_cppwinrt_dir}")
            set(WinRT_FOUND TRUE)
            message(STATUS "WinRT: Found C++/WinRT headers at ${WinRT_INCLUDE_DIR}")
            message(STATUS "WinRT: SDK version directory: ${WinRT_SDK_ROOT}")
            break()
        endif()
    endforeach()
endforeach()

find_package_handle_standard_args(WinRT
    REQUIRED_VARS WinRT_FOUND WinRT_INCLUDE_DIR
    FAIL_MESSAGE "WinRT headers not found. Set WINRT_SDK_ROOT to your Windows SDK Include directory (e.g., 'C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0')")
