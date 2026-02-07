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
        verbose: bool = False
    ):
        self.java_dir = java_dir
        self.csharp_dir = csharp_dir
        self.verbose = verbose

        # 存储分析结果
        self.java_unused: List[str] = []
        self.csharp_unused: List[str] = []
        self.java_used: Set[str] = set()
        self.csharp_used: Set[str] = set()

    def find_swigtype_files(self, directory: Optional[Path], extension: str) -> List[Path]:
        """查找目录中所有SWIGTYPE_p文件"""
        if not directory or not directory.exists():
            return []
        return list(directory.glob(f"SWIGTYPE_p_*{extension}"))

    def analyze_swigtype_usage(self, directory: Optional[Path], extension: str) -> Set[str]:
        """分析目录中所有源文件对SWIGTYPE_p类型的引用"""
        if not directory:
            return set()

        used_types = set()
        pattern = re.compile(r'\b(SWIGTYPE_p(?:_p)?_\w+)\b')

        for source_file in directory.glob(f"*{extension}"):
            if source_file.name.startswith("SWIGTYPE_p_"):
                continue

            try:
                with open(source_file, 'r', encoding='utf-8') as f:
                    content = f.read()
                    matches = pattern.findall(content)
                    used_types.update(matches)
            except Exception as e:
                print(f"Warning: Failed to read {source_file}: {e}", file=sys.stderr)

        return used_types

    def get_unused_files(
        self,
        directory: Optional[Path],
        extension: str,
        used_types: Set[str]
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
            print(f"  Total SWIGTYPE_p files: {len(self.find_swigtype_files(self.java_dir, '.java'))}")
            print(f"  Used types: {len(self.java_used)}")
            print(f"  Unused files: {len(self.java_unused)}")
            total_unused += len(self.java_unused)
            if self.java_unused:
                print(f"  Unused files to delete:")
                for f in self.java_unused:
                    print(f"    - {f}")
            else:
                print(f"  No unused files found.")

        # C#报告
        if self.csharp_dir:
            print(f"\nC# (Directory: {self.csharp_dir}):")
            print(f"  Total SWIGTYPE_p files: {len(self.find_swigtype_files(self.csharp_dir, '.cs'))}")
            print(f"  Used types: {len(self.csharp_used)}")
            print(f"  Unused files: {len(self.csharp_unused)}")
            total_unused += len(self.csharp_unused)
            if self.csharp_unused:
                print(f"  Unused files to delete:")
                for f in self.csharp_unused:
                    print(f"    - {f}")
            else:
                print(f"  No unused files found.")

        print("\n" + "="*60)
        if total_unused > 0:
            java_count = len(self.java_unused) if self.java_dir else 0
            csharp_count = len(self.csharp_unused) if self.csharp_dir else 0
            print(f"\nTotal unused files: {total_unused} (Java: {java_count}, C#: {csharp_count})")
        else:
            print(f"\nNo unused files found.")
        print("="*60)

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

    args = parser.parse_args()

    if args.required and not args.java_dir and not args.csharp_dir:
        parser.error("At least one of --java-dir or --csharp-dir must be specified")

    cleaner = SwigTypeCleaner(
        java_dir=args.java_dir,
        csharp_dir=args.csharp_dir,
        verbose=args.verbose
    )

    # 执行清理
    try:
        deleted_count = cleaner.run(
            delete=args.delete,
            report_file=args.report
        )

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
