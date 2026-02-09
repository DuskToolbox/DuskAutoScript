#!/usr/bin/env python3
"""
清理未使用的SWIGTYPE_p类型文件

扫描SWIG生成的Java和C#代码，分析SWIGTYPE_p类型的使用情况，
删除未被引用的SWIGTYPE_p文件。

Usage:
    python clean_swig_unused_types.py --java-dir <dir> --csharp-dir <dir> [--delete] [--report <file>]
"""

import argparse
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple
import json
from dataclasses import dataclass


@dataclass
class TypeReference:
    """类型引用信息"""
    file: str  # 引用该类型的文件名
    line: int  # 引用所在的行号


def safe_print(text: str) -> None:
    """安全打印，处理Unicode编码错误"""
    try:
        print(text)
    except UnicodeEncodeError:
        # 如果终端不支持Unicode，使用ASCII替代
        ascii_text = text.encode('ascii', 'replace').decode('ascii')
        print(ascii_text)


class SwigTypeCleaner:
    """分析并清理未使用的SWIGTYPE_p类型文件"""

    def __init__(
        self,
        java_dir: Optional[Path],
        csharp_dir: Optional[Path],
        verbose: bool = False,
        debug: bool = False
    ):
        self.java_dir = java_dir
        self.csharp_dir = csharp_dir
        self.verbose = verbose
        self.debug = debug

        # 存储分析结果
        self.java_unused: List[str] = []
        self.csharp_unused: List[str] = []
        self.java_used: Dict[str, List[TypeReference]] = {}
        self.csharp_used: Dict[str, List[TypeReference]] = {}

    def find_swigtype_files(self, directory: Optional[Path], extension: str) -> List[Path]:
        """查找目录中所有SWIGTYPE_p文件"""
        if not directory or not directory.exists():
            return []
        return list(directory.glob(f"SWIGTYPE_p_*{extension}"))

    def analyze_swigtype_usage(self, directory: Optional[Path], extension: str) -> Dict[str, List[TypeReference]]:
        """分析目录中所有源文件对SWIGTYPE_p类型的引用"""
        if not directory:
            return {}

        used_types: Dict[str, List[TypeReference]] = {}
        pattern = re.compile(r'\b(SWIGTYPE_p(?:_p)?_\w+)\b')

        for source_file in directory.glob(f"*{extension}"):
            if source_file.name.startswith("SWIGTYPE_p_"):
                continue

            try:
                with open(source_file, 'r', encoding='utf-8') as f:
                    lines = f.readlines()
                    for line_num, line in enumerate(lines, start=1):
                        matches = pattern.finditer(line)
                        for match in matches:
                            type_name = match.group(1)
                            if type_name not in used_types:
                                used_types[type_name] = []
                            used_types[type_name].append(
                                TypeReference(file=source_file.name, line=line_num)
                            )
            except Exception as e:
                print(f"Warning: Failed to read {source_file}: {e}", file=sys.stderr)

        return used_types

    def get_unused_files(
        self,
        directory: Optional[Path],
        extension: str,
        used_types: Dict[str, List[TypeReference]]
    ) -> List[str]:
        """获取未使用的SWIGTYPE_p文件列表"""
        if not directory:
            return []

        unused_files = []
        swigtype_files = self.find_swigtype_files(directory, extension)

        for swigtype_file in swigtype_files:
            type_name = swigtype_file.stem

            if type_name not in used_types:
                unused_files.append(swigtype_file.name)

        return sorted(unused_files)

    def analyze(self) -> Tuple[List[str], List[str]]:
        """执行分析，返回Java和C#中未使用的文件列表"""
        safe_print(f"Analyzing SWIGTYPE_p usage...")

        # 分析Java
        if self.java_dir:
            safe_print(f"  Scanning Java files in: {self.java_dir}")
            self.java_used = self.analyze_swigtype_usage(self.java_dir, ".java")
            self.java_unused = self.get_unused_files(self.java_dir, ".java", self.java_used)

        # 分析C#
        if self.csharp_dir:
            safe_print(f"  Scanning C# files in: {self.csharp_dir}")
            self.csharp_used = self.analyze_swigtype_usage(self.csharp_dir, ".cs")
            self.csharp_unused = self.get_unused_files(self.csharp_dir, ".cs", self.csharp_used)

        return self.java_unused, self.csharp_unused

    def delete_unused_files(self, unused_files: List[str], directory: Optional[Path]) -> int:
        """删除未使用的文件，返回删除的文件数量"""
        if not directory:
            return 0

        deleted = 0
        for filename in unused_files:
            filepath = directory / filename
            try:
                filepath.unlink()
                if self.verbose:
                    safe_print(f"  Deleted: {filepath}")
                deleted +=1
            except Exception as e:
                safe_print(f"Error: Failed to delete {filepath}: {e}")
        return deleted

    def print_report(self):
        """打印分析报告"""
        print("\n" + "="*60)
        print("SWIGTYPE_p Usage Analysis Report")
        print("="*60)

        total_unused = 0

        # Java报告
        if self.java_dir:
            print(f"\nJava (Directory: {self.java_dir}):")
            if self.java_unused:
                print(f"  Unused files to delete:")
                for f in self.java_unused:
                    print(f"    - {f}")
            else:
                print(f"  No unused files found.")
            total_unused += len(self.java_unused)

        # C#报告
        if self.csharp_dir:
            print(f"\nC# (Directory: {self.csharp_dir}):")
            if self.csharp_unused:
                print(f"  Unused files to delete:")
                for f in self.csharp_unused:
                    print(f"    - {f}")
            else:
                print(f"  No unused files found.")
            total_unused += len(self.csharp_unused)

        print("\n" + "="*60)
        if total_unused > 0:
            if self.java_dir:
                print(f"\nJava:")
                print(f"  Total SWIGTYPE_p files: {len(self.find_swigtype_files(self.java_dir, '.java'))}")
                print(f"  Used types: {len(self.java_used)}")
                print(f"  Unused files: {len(self.java_unused)}")

                if self.debug:
                    print(f"\n  [DEBUG] Used types found in source code:")
                    if self.java_used:
                        for used_type in sorted(self.java_used.keys()):
                            refs = self.java_used[used_type]
                            print(f"    - {used_type}")
                            for ref in refs:
                                print(f"      {ref.file}:{ref.line}")
                    else:
                        print(f"    (none)")

                    print(f"\n  [DEBUG] All SWIGTYPE_p files:")
                    swigtype_files = self.find_swigtype_files(self.java_dir, '.java')
                    for f in sorted(swigtype_files, key=lambda p: p.name):
                        is_used = f.stem in self.java_used
                        status = "USED" if is_used else "UNUSED"
                        print(f"    - {f.name} [{status}]")

            if self.csharp_dir:
                print(f"\nC#:")
                print(f"  Total SWIGTYPE_p files: {len(self.find_swigtype_files(self.csharp_dir, '.cs'))}")
                print(f"  Used types: {len(self.csharp_used)}")
                print(f"  Unused files: {len(self.csharp_unused)}")

                if self.debug:
                    print(f"\n  [DEBUG] Used types found in source code:")
                    if self.csharp_used:
                        for used_type in sorted(self.csharp_used.keys()):
                            refs = self.csharp_used[used_type]
                            print(f"    - {used_type}")
                            for ref in refs:
                                print(f"      {ref.file}:{ref.line}")
                    else:
                        print(f"    (none)")

                    print(f"\n  [DEBUG] All SWIGTYPE_p files:")
                    swigtype_files = self.find_swigtype_files(self.csharp_dir, '.cs')
                    for f in sorted(swigtype_files, key=lambda p: p.name):
                        is_used = f.stem in self.csharp_used
                        status = "USED" if is_used else "UNUSED"
                        print(f"    - {f.name} [{status}]")

            lang_counts = []
            if self.java_dir:
                lang_counts.append(f"Java: {len(self.java_unused)}")
            if self.csharp_dir:
                lang_counts.append(f"C#: {len(self.csharp_unused)}")
            print(f"\nTotal unused files: {total_unused} ({', '.join(lang_counts)})")
        else:
            print(f"\nNo unused files found.")
        print("="*60)

    def print_used_types_warning(self) -> bool:
        """
        打印正在被使用但无法删除的类型警告

        Returns:
            bool: 如果有任何正在被使用的类型，返回True；否则返回False
        """
        has_used_types = False

        # 检查 Java
        if self.java_dir and self.java_used:
            if not has_used_types:
                print("\n" + "!"*60)
                print("WARNING: Found SWIGTYPE_p types that are in use!")
                print("These types cannot be deleted safely.")
                print("!"*60)
                has_used_types = True

            print(f"\nJava used types ({len(self.java_used)}):")
            for type_name, refs in sorted(self.java_used.items()):
                print(f"\n  {type_name}:")
                print(f"    Referenced in {len(refs)} location(s):")
                for ref in refs:
                    print(f"      - {ref.file}:{ref.line}")

        # 检查 C#
        if self.csharp_dir and self.csharp_used:
            if not has_used_types:
                print("\n" + "!"*60)
                print("WARNING: Found SWIGTYPE_p types that are in use!")
                print("These types cannot be deleted safely.")
                print("!"*60)
                has_used_types = True

            print(f"\nC# used types ({len(self.csharp_used)}):")
            for type_name, refs in sorted(self.csharp_used.items()):
                print(f"\n  {type_name}:")
                print(f"    Referenced in {len(refs)} location(s):")
                for ref in refs:
                    print(f"      - {ref.file}:{ref.line}")

        if has_used_types:
            print("\n" + "!"*60)

        return has_used_types

    def generate_json_report(self, report_file: Path):
        """生成JSON格式的报告"""
        report = {}

        if self.java_dir:
            report["java"] = {
                "directory": str(self.java_dir),
                "total_files": len(self.find_swigtype_files(self.java_dir, ".java")),
                "used_types": sorted(list(self.java_used)),
                "unused_files": self.java_unused
            }

        if self.csharp_dir:
            report["csharp"] = {
                "directory": str(self.csharp_dir),
                "total_files": len(self.find_swigtype_files(self.csharp_dir, ".cs")),
                "used_types": sorted(list(self.csharp_used)),
                "unused_files": self.csharp_unused
            }

        with open(report_file, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=2, ensure_ascii=False)

        print(f"\nJSON report saved to: {report_file}")

    def run(self, delete: bool = False, report_file: Optional[Path] = None) -> int:
        """执行清理流程，返回删除的文件总数"""
        # 执行分析
        java_unused, csharp_unused = self.analyze()

        # 打印报告
        self.print_report()

        has_used_types = self.print_used_types_warning()
        if has_used_types:
            return -1

        # 生成JSON报告（如果指定）
        if report_file:
            self.generate_json_report(report_file)

        # 删除未使用的文件（如果指定）
        if delete:
            safe_print("\nDeleting unused files...")
            java_deleted = self.delete_unused_files(java_unused, self.java_dir)
            csharp_deleted = self.delete_unused_files(csharp_unused, self.csharp_dir)

            total_deleted = java_deleted + csharp_deleted
            safe_print(f"\nTotal files deleted: {total_deleted} (Java: {java_deleted}, C#: {csharp_deleted})")

            return total_deleted
        else:
            safe_print("\nDry run mode - no files deleted.")
            safe_print("Use --delete flag to actually delete the files.")
            return 0


def main():
    parser = argparse.ArgumentParser(
        description='Clean unused SWIGTYPE_p type files generated by SWIG',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Dry run (analyze only)
  python clean_swig_unused_types.py --java-dir /path/to/java --csharp-dir /path/to/csharp

  # Delete unused files
  python clean_swig_unused_types.py --java-dir /path/to/java --csharp-dir /path/to/csharp --delete

  # Generate JSON report
  python clean_swig_unused_types.py --java-dir /path/to/java --csharp-dir /path/to/csharp --report report.json

  # Delete and generate report
  python clean_swig_unused_types.py --java-dir /path/to/java --csharp-dir /path/to/csharp --delete --report report.json
        """
    )

    parser.add_argument(
        '--java-dir',
        type=Path,
        help='Directory containing Java SWIG generated files (optional)'
    )

    parser.add_argument(
        '--csharp-dir',
        type=Path,
        help='Directory containing C# SWIG generated files (optional)'
    )

    parser.add_argument(
        '--required',
        type=lambda x: x.lower() in ('true', 'yes', '1', 'on'),
        default=True,
        help='Require at least one language directory to be specified (default: true)'
    )

    parser.add_argument(
        '--delete',
        action='store_true',
        help='Actually delete unused files (default: dry run)'
    )

    parser.add_argument(
        '--report',
        type=Path,
        metavar='FILE',
        help='Generate JSON report to specified file'
    )

    parser.add_argument(
        '-v', '--verbose',
        action='store_true',
        help='Show verbose output'
    )

    parser.add_argument(
        '--debug',
        action='store_true',
        help='Show debug information (used types and all files with status)'
    )

    args = parser.parse_args()

    if args.required and not args.java_dir and not args.csharp_dir:
        parser.error("At least one of --java-dir or --csharp-dir must be specified")

    cleaner = SwigTypeCleaner(
        java_dir=args.java_dir,
        csharp_dir=args.csharp_dir,
        verbose=args.verbose,
        debug=args.debug
    )

    # 执行清理
    try:
        result = cleaner.run(
            delete=args.delete,
            report_file=args.report
        )

        if result == -1:
            return -1

        deleted_count = result

        if args.delete and deleted_count > 0:
            safe_print("\n✓ Cleanup completed successfully!")
        elif not args.delete:
            safe_print("\n✓ Analysis completed successfully!")

        return 0

    except Exception as e:
        safe_print(f"\n✗ Error: {e}")
        return 1


if __name__ == '__main__':
    sys.exit(main())
