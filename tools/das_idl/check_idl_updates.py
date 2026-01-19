#!/usr/bin/env python3
"""
IDL 增量构建检查脚本

检查哪些 IDL 文件需要重新生成代码，只生成需要更新的文件。

用法:
    python check_idl_updates.py --config batch_config.json --updated-list updated_idls.txt

输出:
    updated_idls.txt - 包含需要重新生成的IDL文件列表（每行一个）
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import List, Set, Tuple


def get_file_modtime(filepath: Path) -> float:
    """获取文件的修改时间，如果文件不存在返回0"""
    if not filepath.exists():
        return 0.0
    return filepath.stat().st_mtime


def get_generated_files_for_idl(idl_file: Path, config: dict) -> List[Path]:
    """
    根据配置获取指定IDL文件对应的所有生成文件路径
    
    Args:
        idl_file: IDL文件路径
        config: 该IDL的配置字典
        
    Returns:
        生成文件的路径列表
    """
    idl_name = idl_file.stem
    generated_files = []
    
    # ABI 输出文件（直接在abi输出目录下查找）
    raw_output_dir = Path(config.get("--raw-output-dir", ""))
    if raw_output_dir.exists():
        # ABI文件名包含IDL名称，所以通过文件名过滤
        generated_files.extend(
            f for f in raw_output_dir.glob("*.h")
            if idl_name in f.name
        )
        generated_files.extend(
            f for f in raw_output_dir.glob("*.hpp")
            if idl_name in f.name
        )
    
    
    # Wrapper 输出文件（直接在wrapper输出目录下查找）
    wrapper_output_dir = Path(config.get("--wrapper-output-dir", ""))
    if wrapper_output_dir.exists():
        # Wrapper文件名包含IDL名称，所以通过文件名过滤
        generated_files.extend(
            f for f in wrapper_output_dir.glob("*.h")
            if idl_name in f.name
        )
        generated_files.extend(
            f for f in wrapper_output_dir.glob("*.hpp")
            if idl_name in f.name
        )
        generated_files.extend(
            f for f in wrapper_output_dir.glob("*.cpp")
            if idl_name in f.name
        )
    
    # Implements 输出文件（直接在implements输出目录下查找）
    implements_output_dir = Path(config.get("--implements-output-dir", ""))
    if implements_output_dir.exists() and config.get("--cpp-implements"):
        # Implements文件名包含IDL名称，所以通过文件名过滤
        generated_files.extend(
            f for f in implements_output_dir.glob("*.hpp")
            if idl_name in f.name
        )
    
    # SWIG 输出文件（直接在swig输出目录下查找）
    swig_output_dir = Path(config.get("--swig-output-dir", ""))
    if swig_output_dir.exists() and config.get("--swig"):
        # SWIG文件名包含IDL名称，所以通过文件名过滤
        generated_files.extend(
            f for f in swig_output_dir.glob("*.i")
            if idl_name in f.name
        )
    
    return generated_files


def needs_regeneration(idl_file: Path, generated_files: List[Path], verbose: bool = False) -> bool:
    """
    判断IDL文件是否需要重新生成
    
    Args:
        idl_file: IDL文件路径
        generated_files: 已生成的文件列表
        verbose: 是否输出详细信息
        
    Returns:
        True表示需要重新生成，False表示不需要
    """
    if not idl_file.exists():
        if verbose:
            print(f"  ✗ IDL文件不存在: {idl_file}")
        return True
    
    # 如果没有任何生成文件，需要生成
    if not generated_files:
        if verbose:
            print(f"  ✓ 无生成文件，需要生成: {idl_file}")
        return True
    
    idl_mtime = get_file_modtime(idl_file)
    
    # 检查是否有任何生成文件不存在或比IDL旧
    for gen_file in generated_files:
        if not gen_file.exists():
            if verbose:
                print(f"  ✓ 生成文件缺失，需要生成: {idl_file} -> {gen_file}")
            return True
        
        gen_mtime = get_file_modtime(gen_file)
        if gen_mtime < idl_mtime:
            if verbose:
                print(f"  ✓ 生成文件过期，需要生成: {idl_file} ({idl_mtime}) -> {gen_file} ({gen_mtime})")
            return True
    
    if verbose:
        print(f"  - 所有生成文件都是最新的: {idl_file}")
    return False


def check_tool_file_modified(tool_file: Path, generated_files_dir: Path, verbose: bool = False) -> bool:
    """
    检查工具文件是否被修改（如果工具被修改，所有IDL都需要重新生成）
    
    Args:
        tool_file: 工具文件路径
        generated_files_dir: 所有生成文件的根目录
        verbose: 是否输出详细信息
        
    Returns:
        True表示工具被修改，需要重新生成所有IDL
    """
    if not tool_file.exists():
        return False
    
    tool_mtime = get_file_modtime(tool_file)
    
    # 查找所有生成文件的最新修改时间
    if not generated_files_dir.exists():
        return False
    
    latest_gen_mtime = 0.0
    for gen_file in generated_files_dir.rglob("*"):
        if gen_file.is_file():
            mtime = get_file_modtime(gen_file)
            if mtime > latest_gen_mtime:
                latest_gen_mtime = mtime
    
    if tool_mtime > latest_gen_mtime:
        if verbose:
            print(f"  ⚠ 工具文件已修改，需要重新生成所有IDL: {tool_file}")
        return True
    
    return False


def main():
    parser = argparse.ArgumentParser(
        description='IDL 增量构建检查脚本',
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        '-c', '--config',
        required=True,
        help='批处理配置文件路径'
    )
    parser.add_argument(
        '-o', '--output',
        required=True,
        help='输出文件路径，包含需要重新生成的IDL列表'
    )
    parser.add_argument(
        '-v', '--verbose',
        action='store_true',
        help='显示详细输出'
    )
    parser.add_argument(
        '-f', '--force',
        action='store_true',
        help='强制重新生成所有IDL（忽略增量检查）'
    )
    
    args = parser.parse_args()
    
    # 记录开始时间
    start_time = time.time()
    
    config_path = Path(args.config)
    if not config_path.exists():
        print(f"错误: 配置文件不存在: {config_path}", file=sys.stderr)
        return 1
    
    # 读取配置文件
    try:
        with open(config_path, 'r', encoding='utf-8') as f:
            configs = json.load(f)
    except Exception as e:
        print(f"错误: 读取配置文件失败: {e}", file=sys.stderr)
        return 1
    
    if not isinstance(configs, list):
        print("错误: 配置文件必须包含任务列表", file=sys.stderr)
        return 1
    
    # 工具目录
    tools_dir = Path(__file__).parent
    tool_files = [
        tools_dir / "das_idl_gen.py",
        tools_dir / "das_idl_parser.py",
        tools_dir / "das_cpp_generator.py",
        tools_dir / "das_cpp_wrapper_generator.py",
        tools_dir / "das_swig_generator.py",
    ]
    
    # 检查工具文件是否被修改
    output_dirs = set()
    for config in configs:
        if "--raw-output-dir" in config:
            output_dirs.add(Path(config["--raw-output-dir"]))
        if "--wrapper-output-dir" in config:
            output_dirs.add(Path(config["--wrapper-output-dir"]))
        if "--swig-output-dir" in config:
            output_dirs.add(Path(config["--swig-output-dir"]))
    
    tool_modified = False
    if output_dirs:
        for output_dir in output_dirs:
            for tool_file in tool_files:
                if check_tool_file_modified(tool_file, output_dir, args.verbose):
                    tool_modified = True
                    break
            if tool_modified:
                break
    
    # 如果工具被修改或指定了强制模式，所有IDL都需要重新生成
    if tool_modified or args.force:
        if args.force:
            print("强制模式：需要重新生成所有IDL")
        else:
            print("工具文件已修改，需要重新生成所有IDL")
        updated_idls = [config.get("-i", config.get("--input", "")) for config in configs]
    else:
        # 逐个检查每个IDL文件
        updated_idls = []
        
        if args.verbose:
            print(f"\n检查 {len(configs)} 个IDL文件...")
        
        for config in configs:
            idl_file = Path(config.get("-i", config.get("--input", "")))
            if not idl_file.exists():
                if args.verbose:
                    print(f"  ✗ IDL文件不存在: {idl_file}")
                updated_idls.append(str(idl_file))
                continue
            
            generated_files = get_generated_files_for_idl(idl_file, config)
            
            if needs_regeneration(idl_file, generated_files, args.verbose):
                updated_idls.append(str(idl_file))
    
    # 写入输出文件
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    with open(output_path, 'w', encoding='utf-8') as f:
        for idl_path in updated_idls:
            f.write(f"{idl_path}\n")
    
    if args.verbose:
        print(f"\n{'='*60}")
        print(f"需要重新生成的IDL数量: {len(updated_idls)}/{len(configs)}")
        print(f"更新列表已保存到: {output_path}")
        if len(updated_idls) > 0:
            print(f"\n需要重新生成的IDL文件:")
            for idl_path in updated_idls:
                print(f"  - {idl_path}")
        print(f"{'='*60}")
    
    # 计算并输出总耗时
    end_time = time.time()
    elapsed_time = end_time - start_time
    
    print(f"\n{'='*60}")
    print(f"IDL增量检查完成")
    print(f"  总任务数: {len(configs)}")
    print(f"  需要更新: {len(updated_idls)}")
    print(f"  跳过任务: {len(configs) - len(updated_idls)}")
    print(f"  总耗时: {elapsed_time:.3f}秒")
    print(f"{'='*60}")
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
