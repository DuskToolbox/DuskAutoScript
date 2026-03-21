#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
IDL 增量构建检查脚本

检查哪些 IDL 文件需要重新生成代码，只生成需要更新的文件。

用法:
    python check_idl_updates.py --config batch_config.json --updated-list updated_idls.txt

输出:
    updated_idls.txt - 包含需要重新生成的IDL文件列表（每行一个）
"""

import argparse
import glob
import json
import os
import sys
import time
from pathlib import Path
from typing import List, Set, Tuple


def safe_print(text: str) -> None:
    """安全打印，处理Unicode编码错误"""
    try:
        print(text)
    except UnicodeEncodeError:
        # 如果终端不支持Unicode，使用ASCII替代
        ascii_text = text.encode('ascii', 'replace').decode('ascii')
        print(ascii_text)


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

    # IPC Proxy/Stub 输出文件
    ipc_output_dir = Path(config.get("--ipc-output-dir", ""))
    if ipc_output_dir != Path(""):
        if config.get("--ipc-proxy"):
            proxy_file = ipc_output_dir / "proxy" / f"{idl_name}Proxy.h"
            generated_files.append(proxy_file)
        if config.get("--ipc-stub"):
            stub_file = ipc_output_dir / "stub" / f"{idl_name}Stub.h"
            generated_files.append(stub_file)

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


def check_tool_file_modified(tool_file: Path, generated_files_dir: Path) -> Tuple[bool, float, float]:
    """
    检查工具文件是否被修改（如果工具被修改，所有IDL都需要重新生成）
    
    Args:
        tool_file: 工具文件路径
        generated_files_dir: 所有生成文件的根目录
        
    Returns:
        Tuple[是否修改, 工具文件修改时间, 最新生成文件修改时间]
    """
    if not tool_file.exists():
        return False, 0.0, 0.0
    
    tool_mtime = get_file_modtime(tool_file)
    
    # 查找所有生成文件的最新修改时间
    if not generated_files_dir.exists():
        return False, tool_mtime, 0.0
    
    latest_gen_mtime = 0.0
    for gen_file in generated_files_dir.rglob("*"):
        if gen_file.is_file():
            mtime = get_file_modtime(gen_file)
            if mtime > latest_gen_mtime:
                latest_gen_mtime = mtime
    
    is_modified = tool_mtime > latest_gen_mtime
    return is_modified, tool_mtime, latest_gen_mtime


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
    
    # 工具目录 - 使用glob自动收集所有Python脚本
    tools_dir = Path(__file__).parent
    tool_files = [Path(f) for f in glob.glob(str(tools_dir / "*.py"))]
    
    # 检查工具文件是否被修改
    output_dirs = set()
    for config in configs:
        if "--raw-output-dir" in config:
            output_dirs.add(Path(config["--raw-output-dir"]))
        if "--wrapper-output-dir" in config:
            output_dirs.add(Path(config["--wrapper-output-dir"]))
        if "--swig-output-dir" in config:
            output_dirs.add(Path(config["--swig-output-dir"]))
    
    # 检查工具文件修改情况
    modified_tools = []
    if output_dirs:
        for output_dir in output_dirs:
            for tool_file in tool_files:
                is_modified, tool_mtime, latest_gen_mtime = check_tool_file_modified(tool_file, output_dir)
                if is_modified:
                    modified_tools.append({
                        'file': tool_file,
                        'tool_mtime': tool_mtime,
                        'gen_mtime': latest_gen_mtime
                    })
    
    tool_modified = len(modified_tools) > 0
    
    # 检查全局聚合文件是否存在
    # 如果聚合文件缺失，所有 IDL 都需要重新生成（例如 ninja clean 后）
    aggregation_files_missing = False
    ipc_output_dirs = set()
    for config in configs:
        ipc_dir = config.get("--ipc-output-dir", "")
        if ipc_dir and (config.get("--ipc-proxy") or config.get("--ipc-stub")):
            ipc_output_dirs.add(Path(ipc_dir))

    for ipc_dir in ipc_output_dirs:
        for aggregation_file in ["IpcGenerated.cpp", "IpcAllProxies.h", "IpcAllStubs.h"]:
            if not (ipc_dir / aggregation_file).exists():
                aggregation_files_missing = True
                if args.verbose:
                    safe_print(f"  聚合文件缺失: {ipc_dir / aggregation_file}")
                break
        if aggregation_files_missing:
            break

    # 如果工具被修改、指定了强制模式或聚合文件缺失，所有IDL都需要重新生成
    if tool_modified or args.force or aggregation_files_missing:
        if args.force:
            print("强制模式：需要重新生成所有IDL")
        elif aggregation_files_missing:
            print("聚合文件缺失，需要重新生成所有IDL")
        else:
            print(f"\n检测到 {len(modified_tools)} 个工具文件已修改，需要重新生成所有IDL:")
            print("-" * 80)
            for info in modified_tools:
                tool_time_str = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(info['tool_mtime']))
                gen_time_str = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(info['gen_mtime'])) if info['gen_mtime'] > 0 else 'N/A'
                safe_print(f"  📄 {info['file'].name}")
                safe_print(f"     工具修改时间: {tool_time_str}")
                safe_print(f"     生成文件时间: {gen_time_str}")
            print("-" * 80)
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
