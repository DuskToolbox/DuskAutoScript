#!/usr/bin/env python3
"""
获取VS开发环境配置
"""
import os
import re
from pathlib import Path


def get_vs_env_script_from_compiler(compiler_path):
    """从编译器路径获取VS环境脚本路径

    Args:
        compiler_path: cl.exe的路径，如 "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.50.35717/bin/Hostx64/x64/cl.exe"

    Returns:
        VS环境脚本路径，如 "C:/Program Files/Microsoft Visual Studio/18/Community/Common7/Tools/VsDevCmd.bat"
        如果不是MSVC编译器，返回None
    """
    if not compiler_path or 'cl.exe' not in compiler_path.lower():
        return None

    # 转换为Path对象并规范化
    compiler_path = Path(compiler_path).resolve()

    # 从编译器路径提取VS安装根目录
    # 路径结构: <VS_ROOT>/VC/Tools/MSVC/<version>/bin/Hostx64/x64/cl.exe
    # 我们需要提取 <VS_ROOT>

    # 方法1: 查找VC目录的父级
    parts = compiler_path.parts

    # 查找 "VC" 的索引
    try:
        vc_index = parts.index('VC')
        vs_root_parts = parts[:vc_index]
        vs_root = Path(*vs_root_parts)
    except (ValueError, IndexError):
        # 方法2: 使用正则表达式提取
        # cl.exe通常在: .../VC/Tools/MSVC/.../bin/.../cl.exe
        # 我们需要提取到 VC 之前的路径
        match = re.search(r'^(.*?)[/\\]VC[/\\]Tools[/\\]MSVC', str(compiler_path))
        if match:
            vs_root = Path(match.group(1))
        else:
            return None

    # 构建VS环境脚本路径
    vsdevcmd_path = vs_root / 'Common7' / 'Tools' / 'VsDevCmd.bat'

    if vsdevcmd_path.exists():
        return str(vsdevcmd_path).replace('\\', '/')

    # 尝试其他可能的路径
    vcvars_path = vs_root / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat'
    if vcvars_path.exists():
        return str(vcvars_path)

    return None


def get_compiler_from_cmake_cache(build_dir):
    """从CMake缓存中获取编译器路径

    Args:
        build_dir: 构建目录

    Returns:
        编译器路径字符串
    """
    cache_file = Path(build_dir) / 'CMakeCache.txt'
    if not cache_file.exists():
        return None

    try:
        with open(cache_file, 'r', encoding='utf-8') as f:
            for line in f:
                if line.startswith('CMAKE_CXX_COMPILER:FILEPATH='):
                    return line.split('=', 1)[1].strip()
    except Exception as e:
        print(f"警告: 读取CMake缓存失败: {e}")

    return None


def get_vs_env_from_build_dir(build_dir):
    """从构建目录获取VS环境配置

    Args:
        build_dir: 构建目录

    Returns:
        (vs_script_path, arch) 元组
        vs_script_path: VS环境脚本路径，如果不是MSVC则为None
        arch: 目标架构，如 "x64", "x86"
    """
    compiler_path = get_compiler_from_cmake_cache(build_dir)

    if not compiler_path:
        print("无法从CMake缓存获取编译器信息")
        return None, None

    vs_script = get_vs_env_script_from_compiler(compiler_path)

    if not vs_script:
        print(f"编译器不是MSVC或无法找到VS环境脚本: {compiler_path}")
        return None, None

    # 从编译器路径推断目标架构
    # 通常在 bin/Hostx64/x64/ 或 bin/Hostx64/x86/
    if 'x64' in compiler_path.lower():
        arch = 'x64'
    elif 'x86' in compiler_path.lower() or 'arm64' not in compiler_path.lower():
        arch = 'x86'
    else:
        arch = 'x64'  # 默认

    return vs_script, arch


if __name__ == '__main__':
    import sys

    if len(sys.argv) > 1:
        build_dir = sys.argv[1]
    else:
        build_dir = 'C:/vmbuild'

    vs_script, arch = get_vs_env_from_build_dir(build_dir)

    if vs_script:
        print(f"VS环境脚本: {vs_script}")
        print(f"目标架构: {arch}")
    else:
        print("未找到VS环境配置")
        sys.exit(1)
