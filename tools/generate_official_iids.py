#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
IDL文件扫描器 - 生成官方IID列表
扫描指定的IDL文件，生成OfficialIids.h和OfficialIids.cpp文件
这些文件包含所有接口的GUID集合(std::set<DasGuid>)
"""

import sys
import os
import argparse
from pathlib import Path
from typing import Set, Dict
import hashlib

# 添加das_idl模块路径
script_dir = Path(__file__).parent
sys.path.insert(0, str(script_dir / 'das_idl'))

from das_idl_parser import parse_idl_file, IdlDocument, InterfaceDef


class IdlScanner:
    """IDL文件扫描器"""
    
    def __init__(self):
        self.interfaces: Dict[str, str] = {}  # 接口名 -> UUID
        self.namespace_map: Dict[str, str] = {}  # 接口名 -> 命名空间
        self.guid_constants: Dict[str, str] = {}  # 接口名 -> GUID常量名
        self.idl_files: Set[str] = set()  # IDL文件名集合（不含扩展名）
    
    def scan_file(self, idl_file: Path) -> bool:
        """扫描单个IDL文件"""
        if not idl_file.exists():
            print(f"警告: 文件不存在: {idl_file}")
            return False
        
        try:
            # 记录IDL文件名（不含扩展名）
            self.idl_files.add(idl_file.stem)
            
            doc = parse_idl_file(str(idl_file))
            
            # 收集所有接口
            for interface in doc.interfaces:
                if interface.uuid:
                    # 构建完整的接口名（包含命名空间）
                    full_name = self._get_full_interface_name(interface)
                    self.interfaces[full_name] = interface.uuid
                    self.namespace_map[full_name] = interface.namespace or ""
                    # 生成GUID常量名
                    guid_constant = self._generate_guid_constant_name(interface)
                    self.guid_constants[full_name] = guid_constant
                    
            return True
        except Exception as e:
            print(f"错误: 解析文件 {idl_file} 失败: {e}")
            return False
    
    def _get_full_interface_name(self, interface: InterfaceDef) -> str:
        """获取完整的接口名（包含命名空间）"""
        if interface.namespace:
            return f"{interface.namespace}::{interface.name}"
        return interface.name
    
    def _generate_guid_constant_name(self, interface: InterfaceDef) -> str:
        """生成GUID常量名，类似于DAS_IID_XXX"""
        # 将接口名转换为常量名格式
        # 例如: IDasCV -> DAS_IID_C_V
        # IDasTemplateMatchResult -> DAS_IID_TEMPLATE_MATCH_RESULT
        # IDasCaptureManager -> DAS_IID_CAPTURE_MANAGER
        name = interface.name
        # 移除前缀'I'
        if name.startswith('I'):
            name = name[4:]
        # 转换为蛇形命名并大写
        import re
        # 处理连续大写字母的情况（如 "CV" 在 IDasCV 中）
        # 在大写字母前插入下划线，但不在连续大写字母之间插入
        name = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2', name)
        # 在小写字母后的大写字母前插入下划线
        name = re.sub(r'([a-z\d])([A-Z])', r'\1_\2', name)
        # 全部大写
        name = name.upper()
        return f"DAS_IID_{name}"
    
    def scan_files(self, idl_files: list) -> int:
        """扫描多个IDL文件"""
        success_count = 0
        for idl_file in idl_files:
            idl_path = Path(idl_file)
            if self.scan_file(idl_path):
                success_count += 1
        return success_count
    
    def generate_header(self, output_path: Path) -> None:
        """生成OfficialIids.h头文件"""
        output_path.parent.mkdir(parents=True, exist_ok=True)

        with open(output_path, 'w', encoding='utf-8') as f:
            f.write('''// 本文件由IDL扫描器自动生成，请勿手动编辑
// 生成时间: {timestamp}

#ifndef DAS_AUTOGEN_OFFICIAL_IIDS_H
#define DAS_AUTOGEN_OFFICIAL_IIDS_H

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <unordered_set>

namespace Das {{
    namespace _autogen {{
        /// 官方接口GUID集合
        /// 包含所有在IDL文件中定义的接口的GUID
        extern const std::unordered_set<DasGuid> g_official_iids;
    }}
}}

#endif // DAS_AUTOGEN_OFFICIAL_IIDS_H
'''.format(
    timestamp=__import__('datetime').datetime.now().strftime('%Y-%m-%d %H:%M:%S')
))
        print(f"已生成: {output_path}")
    
    def generate_source(self, output_path: Path) -> None:
        """生成OfficialIids.cpp源文件"""
        output_path.parent.mkdir(parents=True, exist_ok=True)

        # 收集所有接口并按命名空间和接口名排序
        sorted_interfaces = sorted(self.interfaces.keys())
        # 对IDL文件名排序
        sorted_idl_files = sorted(self.idl_files)

        with open(output_path, 'w', encoding='utf-8') as f:
            f.write('''// 本文件由IDL扫描器自动生成，请勿手动编辑
// 生成时间: {timestamp}

#include "OfficialIids.h"

'''.format(
    timestamp=__import__('datetime').datetime.now().strftime('%Y-%m-%d %H:%M:%S')
))

            # 添加所有IDL对应的abi目录下的头文件包含
            for idl_name in sorted_idl_files:
                f.write(f'#include "{idl_name}.h"\n')
            f.write('''
namespace Das {
    namespace _autogen {
        const std::unordered_set<DasGuid> g_official_iids{
''')

            # 添加所有GUID常量引用
            for interface_name in sorted_interfaces:
                guid_constant = self.guid_constants.get(interface_name, "UNKNOWN_GUID")
                f.write(f'            {guid_constant},\n')

            f.write('''        };
    }
}
''')
        print(f"已生成: {output_path}")
        print(f"  共收集 {len(sorted_interfaces)} 个接口")
        print(f"  共包含 {len(sorted_idl_files)} 个IDL头文件")


def compute_file_hash(file_path: Path) -> str:
    """计算文件的哈希值"""
    if not file_path.exists():
        return ""
    
    hasher = hashlib.md5()
    with open(file_path, 'rb') as f:
        while chunk := f.read(8192):
            hasher.update(chunk)
    return hasher.hexdigest()


def main():
    parser = argparse.ArgumentParser(
        description='IDL文件扫描器 - 生成官方IID列表',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
示例:
  %(prog)s -i idl/ExportInterface -o das/Core/ForeignInterfaceHost/autogen
  %(prog)s -i idl1.idl idl2.idl -o output_dir
  %(prog)s -i idl/ExportInterface -o output_dir --check-deps deps.txt
        '''
    )
    
    parser.add_argument(
        '-i', '--input',
        nargs='+',
        required=True,
        help='输入的IDL文件或目录'
    )
    
    parser.add_argument(
        '-o', '--output',
        required=True,
        help='输出目录'
    )
    
    parser.add_argument(
        '--check-deps',
        help='依赖文件路径（用于增量构建检查）'
    )
    
    parser.add_argument(
        '-v', '--verbose',
        action='store_true',
        help='显示详细输出'
    )
    
    args = parser.parse_args()
    
    # 收集所有IDL文件
    idl_files = []
    for input_path in args.input:
        path = Path(input_path)
        if path.is_dir():
            idl_files.extend(path.glob('**/*.idl'))
        elif path.is_file():
            idl_files.append(path)
        else:
            print(f"错误: 路径不存在: {input_path}")
            return 1
    
    if not idl_files:
        print("错误: 未找到任何IDL文件")
        return 1
    
    if args.verbose:
        print(f"找到 {len(idl_files)} 个IDL文件")
        for idl_file in idl_files:
            print(f"  - {idl_file}")
    
    # 检查依赖（增量构建支持）
    if args.check_deps:
        deps_file = Path(args.check_deps)
        if deps_file.exists():
            with open(deps_file, 'r', encoding='utf-8') as f:
                saved_hash = f.read().strip()
            
            # 计算当前所有IDL文件的哈希
            current_hashes = []
            for idl_file in sorted(idl_files):
                current_hashes.append(compute_file_hash(idl_file))
            combined_hash = hashlib.md5(''.join(current_hashes).encode()).hexdigest()
            
            if saved_hash == combined_hash:
                if args.verbose:
                    print("IDL文件未发生变化，跳过生成")
                # 触摸输出文件以更新时间戳，让CMake知道我们已经检查过了
                output_dir = Path(args.output)
                header_file = output_dir / 'OfficialIids.h'
                source_file = output_dir / 'OfficialIids.cpp'
                if header_file.exists():
                    header_file.touch()
                if source_file.exists():
                    source_file.touch()
                return 0
    
    # 扫描IDL文件
    scanner = IdlScanner()
    success_count = scanner.scan_files(idl_files)
    
    if success_count == 0:
        print("错误: 未能成功解析任何IDL文件")
        return 1
    
    if args.verbose:
        print(f"成功解析 {success_count} 个IDL文件")
    
    # 生成输出文件
    output_dir = Path(args.output)
    header_file = output_dir / 'OfficialIids.h'
    source_file = output_dir / 'OfficialIids.cpp'
    
    scanner.generate_header(header_file)
    scanner.generate_source(source_file)
    
    # 保存依赖哈希（用于增量构建）
    if args.check_deps:
        deps_file = Path(args.check_deps)
        deps_file.parent.mkdir(parents=True, exist_ok=True)
        current_hashes = []
        for idl_file in sorted(idl_files):
            current_hashes.append(compute_file_hash(idl_file))
        combined_hash = hashlib.md5(''.join(current_hashes).encode()).hexdigest()
        
        with open(deps_file, 'w', encoding='utf-8') as f:
            f.write(combined_hash)
        
        if args.verbose:
            print(f"已保存依赖哈希: {deps_file}")
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
