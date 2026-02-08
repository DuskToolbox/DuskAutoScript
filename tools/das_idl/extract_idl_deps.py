#!/usr/bin/env python3
"""
从 IDL 文件中提取接口继承关系

此脚本解析 IDL 文件，提取所有接口定义及其基类关系，
生成 JSON 格式的依赖关系文件，用于后续的拓扑排序。
"""

import os
import re
import json
import argparse
import time
from pathlib import Path
from typing import Dict, List, Set, Optional


class IDLInterface:
    """IDL 接口定义"""
    
    def __init__(self, name: str, base: Optional[str] = None, file_path: str = ""):
        self.name = name
        self.base = base or "IDasBase"  # 默认基类为 IDasBase
        self.file_path = file_path
        self.uuid: Optional[str] = None
    
    def to_dict(self) -> dict:
        """转换为字典"""
        return {
            "name": self.name,
            "base": self.base,
            "uuid": self.uuid,
            "file": self.file_path
        }


class IDLParser:
    """IDL 文件解析器"""
    
    # 匹配 interface 定义的正则表达式
    # 支持两种格式：
    # 1. [uuid("...")] interface Name : Base { ... }
    # 2. interface Name : Base { ... }
    INTERFACE_PATTERN = re.compile(
        r'''
        ^\s*
        (?:\[uuid\(["'](?P<uuid>[^"']+)["']\)\]\s*)?  # 可选的 uuid 属性
        interface\s+
        (?P<name>\w+)\s*
        (?:\:\s*(?P<base>\w+))?                        # 可选的基类
        \s*\{                                            # 开始大括号
        ''',
        re.VERBOSE | re.MULTILINE
    )
    
    # 匹配 import 语句
    IMPORT_PATTERN = re.compile(r'^\s*import\s+"([^"]+\.idl)";', re.MULTILINE)
    
    def __init__(self, idl_dir: str):
        self.idl_dir = Path(idl_dir)
        self.interfaces: Dict[str, IDLInterface] = {}
        self.imports: Dict[str, List[str]] = {}  # 文件 -> 导入的文件列表
    
    def parse_file(self, file_path: Path) -> None:
        """解析单个 IDL 文件"""
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
        except Exception as e:
            print(f"警告: 无法读取文件 {file_path}: {e}")
            return
        
        file_name = file_path.name
        self.imports[file_name] = []
        
        # 提取 import 语句
        for match in self.IMPORT_PATTERN.finditer(content):
            imported_file = match.group(1)
            self.imports[file_name].append(imported_file)
        
        # 提取接口定义
        for match in self.INTERFACE_PATTERN.finditer(content):
            name = match.group('name')
            base = match.group('base')
            uuid = match.group('uuid')
            
            # 创建接口对象
            interface = IDLInterface(name, base, str(file_path))
            interface.uuid = uuid
            
            # 存储接口
            if name in self.interfaces:
                print(f"警告: 接口 {name} 在多个文件中定义")
                print(f"  已定义: {self.interfaces[name].file_path}")
                print(f"  新定义: {file_path}")
            
            self.interfaces[name] = interface
    
    def parse_all(self) -> None:
        """解析所有 IDL 文件"""
        if not self.idl_dir.exists():
            raise FileNotFoundError(f"IDL 目录不存在: {self.idl_dir}")
        
        # 遍历所有 .idl 文件
        for idl_file in self.idl_dir.glob('*.idl'):
            print(f"解析文件: {idl_file.name}")
            self.parse_file(idl_file)
        
        print(f"\n共找到 {len(self.interfaces)} 个接口")
    
    def _would_create_cycle(self, graph: Dict[str, Set[str]], node: str, target: str) -> bool:
        """检查添加 node -> target 依赖是否会形成环"""
        if node == target:
            return True

        visited: Set[str] = set()
        stack: List[str] = [target]

        while stack:
            current = stack.pop()
            if current == node:
                return True
            if current in visited:
                continue
            visited.add(current)
            stack.extend(graph.get(current, set()))

        return False

    def get_dependency_graph(self) -> Dict[str, List[str]]:
        """获取依赖关系图（用于拓扑排序）

        依赖关系语义：
        - 如果 interface A 继承自 interface B，则 A 依赖于 B
        - graph[A] 包含 B，表示 A 依赖 B
        - 拓扑排序时，B 应该在 A 之前出现
        - 如果文件 A import 了文件 B，而文件 B 中定义了接口，那么文件 A 中的接口也依赖这些接口
        """
        graph: Dict[str, Set[str]] = {name: set() for name in self.interfaces.keys()}

        file_to_interfaces: Dict[str, Set[str]] = {}
        for name, interface in self.interfaces.items():
            file_name = Path(interface.file_path).name
            if file_name not in file_to_interfaces:
                file_to_interfaces[file_name] = set()
            file_to_interfaces[file_name].add(name)

        for name, interface in self.interfaces.items():
            base = interface.base
            if base and not self._would_create_cycle(graph, name, base):
                graph[name].add(base)
            if base not in graph:
                graph[base] = set()

        for file_name, imported_files in self.imports.items():
            if file_name not in file_to_interfaces:
                continue
            for imported_file in imported_files:
                if imported_file not in file_to_interfaces:
                    continue
                imported_interfaces = file_to_interfaces[imported_file]
                for interface_name in file_to_interfaces[file_name]:
                    for dep in imported_interfaces:
                        if not self._would_create_cycle(graph, interface_name, dep):
                            graph[interface_name].add(dep)

        return {name: sorted(list(deps)) for name, deps in graph.items()}
    
    def save_to_json(self, output_path: str) -> None:
        """保存结果到 JSON 文件"""
        output = {
            "interfaces": [
                interface.to_dict()
                for interface in sorted(self.interfaces.values(), key=lambda x: x.name)
            ],
            "imports": self.imports,
            "dependency_graph": self.get_dependency_graph()
        }
        
        output_file = Path(output_path)
        output_file.write_text(
            json.dumps(output, indent=2, ensure_ascii=False),
            encoding='utf-8'
        )
        
        print(f"\n依赖关系已保存到: {output_path}")
    
    def print_summary(self) -> None:
        """打印摘要信息"""
        print("\n=== 接口列表 ===")
        for interface in sorted(self.interfaces.values(), key=lambda x: x.name):
            base = interface.base if interface.base else "IDasBase"
            uuid_str = f" {interface.uuid[:8]}..." if interface.uuid else " 无uuid"
            print(f"  {interface.name:30s} : {base:30s} [{uuid_str}]")
        
        print(f"\n=== 统计信息 ===")
        print(f"  总接口数: {len(self.interfaces)}")
        print(f"  带 uuid 的接口: {sum(1 for i in self.interfaces.values() if i.uuid)}")
        
        # 统计基类
        base_counts: Dict[str, int] = {}
        for interface in self.interfaces.values():
            base = interface.base
            base_counts[base] = base_counts.get(base, 0) + 1
        
        print("\n=== 基类统计 ===")
        for base, count in sorted(base_counts.items(), key=lambda x: -x[1]):
            print(f"  {base:30s} : {count:3d}")


def main():
    parser = argparse.ArgumentParser(
        description="从 IDL 文件中提取接口继承关系",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python tools/extract_idl_deps.py
  python tools/extract_idl_deps.py --idl-dir idl --output swig_deps.json
  python tools/extract_idl_deps.py --verbose
        """
    )

    parser.add_argument(
        '--idl-dir',
        default='idl',
        help='IDL 文件所在目录 (默认: idl)'
    )

    parser.add_argument(
        '--output',
        default='swig_deps.json',
        help='输出 JSON 文件路径 (默认: swig_deps.json)'
    )

    parser.add_argument(
        '-v', '--verbose',
        action='store_true',
        help='显示详细信息'
    )

    args = parser.parse_args()

    print("=" * 60)
    print("IDL 接口依赖关系提取工具")
    print("=" * 60)
    print(f"IDL 目录: {args.idl_dir}")
    print(f"输出文件: {args.output}")
    print()

    start_time = time.time()

    try:
        # 解析 IDL 文件
        idl_parser = IDLParser(args.idl_dir)
        idl_parser.parse_all()

        # 保存到 JSON
        idl_parser.save_to_json(args.output)

        # 打印摘要
        if args.verbose:
            idl_parser.print_summary()

        end_time = time.time()
        elapsed_ms = int((end_time - start_time) * 1000)

        print(f"\n[OK] 完成! 耗时: {elapsed_ms} ms")

    except Exception as e:
        print(f"\n[ERROR] 错误: {e}")
        import traceback
        traceback.print_exc()
        return 1

    return 0


if __name__ == '__main__':
    exit(main())
