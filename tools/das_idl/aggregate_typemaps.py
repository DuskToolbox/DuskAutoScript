#!/usr/bin/env python3
"""
Typemap 聚合脚本

用法:
    python aggregate_typemaps.py --swig-output-dir <dir> [--output-file <filename>]

示例:
    python aggregate_typemaps.py --swig-output-dir C:/vmbuild/das/include/das/_autogen/idl/swig
    python aggregate_typemaps.py --swig-output-dir ./swig --output-file DasTypeMaps.i
"""

import argparse
import json
import sys
import time
from pathlib import Path


def aggregate_typemaps(swig_output_dir: Path, output_filename: str = "DasTypeMaps.i") -> int:
    """
    从所有 typemap_info_*.json 文件汇总生成 DasTypeMaps.i
    
    Args:
        swig_output_dir: SWIG 输出目录（包含 typemap_info_*.json 文件）
        output_filename: 输出文件名，默认为 DasTypeMaps.i
    
    Returns:
        0 表示成功，非0 表示失败
    """
    sys.path.insert(0, str(Path(__file__).parent))
    
    from das_idl_gen import generate_type_maps_from_jsons
    
    # 查找所有 typemap_info JSON 文件
    json_files = list(swig_output_dir.glob("typemap_info_*.json"))

    if not json_files:
        print(f"No typemap_info JSON files found in {swig_output_dir}")
        return 0  # 没有文件也是成功的

    print(f"Found {len(json_files)} typemap_info JSON files")

    start_time = time.time()

    try:
        result = generate_type_maps_from_jsons(
            [str(f) for f in json_files],
            output_filename,
            swig_output_dir
        )

        end_time = time.time()
        elapsed_ms = int((end_time - start_time) * 1000)

        if result is None:
            print(f"Successfully generated {output_filename} in {elapsed_ms} ms")
            return 0
        else:
            print(f"Unexpected result: {result}")
            return 1

    except Exception as e:
        print(f"Error aggregating typemaps: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1


def main():
    parser = argparse.ArgumentParser(
        description='Aggregate typemaps from typemap_info JSON files',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s --swig-output-dir C:/vmbuild/das/include/das/_autogen/idl/swig
  %(prog)s --swig-output-dir ./swig --output-file DasTypeMaps.i
"""
    )
    
    parser.add_argument(
        '--swig-output-dir',
        required=True,
        help='SWIG 输出目录（包含 typemap_info_*.json 文件）'
    )
    
    parser.add_argument(
        '--output-file',
        default='DasTypeMaps.i',
        help='输出文件名（默认: DasTypeMaps.i）'
    )
    
    args = parser.parse_args()
    
    swig_output_dir = Path(args.swig_output_dir)
    
    if not swig_output_dir.exists():
        print(f"Error: Directory does not exist: {swig_output_dir}", file=sys.stderr)
        return 1
    
    if not swig_output_dir.is_dir():
        print(f"Error: Not a directory: {swig_output_dir}", file=sys.stderr)
        return 1
    
    return aggregate_typemaps(swig_output_dir, args.output_file)


if __name__ == '__main__':
    sys.exit(main())
