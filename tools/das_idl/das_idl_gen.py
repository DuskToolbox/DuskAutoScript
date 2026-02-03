#!/usr/bin/env python3
"""
DAS IDL 代码生成器入口脚本

用法:
    python das_idl_gen.py --input <idl_file> --output-dir <output_dir> [options]

示例:
    python das_idl_gen.py --input interfaces.idl --output-dir ./generated
    python das_idl_gen.py -i interfaces.idl -o ./generated -n DAS --swig
    python das_idl_gen.py -i interfaces.idl --raw-output-dir ./raw --wrapper-output-dir ./wrapper --swig-output-dir ./swig

生成内容:
    1. C++ 头文件 (IDasXxx.h) - 原始接口定义
    2. C++ 包装头文件 (Das.Xxx.hpp) - C++/WinRT 风格的便捷包装类
    3. C++ 实现基类模板 (Das.Xxx.Implements.hpp) - 类似 winrt::implements 的实现基类
    4. SWIG .i 文件 (IDasXxx.i) - 每个接口的 SWIG 配置
    5. 汇总 .i 文件 (base_name_all.i) - include 所有接口的 .i 文件
"""

import argparse
import os
import sys
from pathlib import Path
from typing import List, Optional


def main():
    parser = argparse.ArgumentParser(
        description='DAS IDL Code Generator - 从 IDL 文件生成 C++ 接口代码和 SWIG 配置',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s --input interfaces.idl --output-dir ./generated
  %(prog)s -i interfaces.idl -o ./generated --namespace DAS
  %(prog)s -i interfaces.idl -o ./generated --swig --cpp-wrapper

  # 将不同类型的文件输出到不同目录
  %(prog)s -i interfaces.idl \\
      --raw-output-dir ./include/das/raw \\
      --wrapper-output-dir ./include/das/wrapper \\
      --swig-output-dir ./swig

IDL 语法示例:
  // 枚举定义
  enum MyEnum {
      Value1 = 0,
      Value2,
  }

  // 接口定义
  [uuid("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx")]
  interface IDasMyInterface : IDasBase {
      DasResult DoSomething([out] IOutputType** pp_out);
      [get, set] int32 PropertyName
  }

  // 嵌套命名空间定义（支持 C++17 语法）
  namespace DAS::ExportInterface {
      enum MyEnum { Value1 = 0 }

      [uuid("xxx")]
      interface IDasMyInterface : IDasBase {
          DasResult DoSomething([out] IOutputType** pp_out);
      }
  }
"""
    )
    parser.add_argument(
        '-i', '--input',
        required=True,
        help='输入的 IDL 文件路径（支持多个文件，用逗号分隔）'
    )

    # === 输出目录选项 ===
    output_group = parser.add_argument_group('输出目录选项')

    output_group.add_argument(
        '-o', '--output-dir',
        help='通用输出目录（当未指定具体类型的输出目录时使用）'
    )

    output_group.add_argument(
        '--raw-output-dir',
        help='原始 C++ 头文件 (IDasXxx.h) 的输出目录'
    )

    output_group.add_argument(
        '--wrapper-output-dir',
        help='C++ 包装文件 (Das.Xxx.hpp) 的输出目录'
    )

    output_group.add_argument(
        '--implements-output-dir',
        help='C++ 实现基类模板文件 (Das.Xxx.Implements.hpp) 的输出目录'
    )

    output_group.add_argument(
        '--swig-output-dir',
        help='SWIG .i 文件的输出目录'
    )

    # === 生成选项 ===
    gen_group = parser.add_argument_group('生成选项')

    gen_group.add_argument(
        '-n', '--namespace',
        default='',
        help='C++ 命名空间 (可选)'
    )

    gen_group.add_argument(
        '--wrapper-namespace',
        default='Das',
        help='C++ 包装类的命名空间 (默认: Das)'
    )

    gen_group.add_argument(
        '--base-name',
        help='生成文件的基础名称，默认使用 IDL 文件名'
    )

    gen_group.add_argument(
        '--swig',
        action='store_true',
        help='生成 SWIG .i 文件'
    )

    gen_group.add_argument(
        '--cpp-wrapper',
        action='store_true',
        help='生成 C++/WinRT 风格的包装文件 (Das.Xxx.hpp)'
    )

    gen_group.add_argument(
        '--cpp-implements',
        action='store_true',
        help='生成 C++ 实现基类模板文件 (Das.Xxx.Implements.hpp)，类似 winrt::implements'
    )

    gen_group.add_argument(
        '--all',
        action='store_true',
        help='生成所有类型的文件（等同于 --swig --cpp-wrapper --cpp-implements）'
    )

    gen_group.add_argument(
        '--generate-type-maps',
        action='store_true',
        help='生成typemap_info.json文件（用于汇总生成DasTypeMaps.i）'
    )

    # === 调试选项 ===
    debug_group = parser.add_argument_group('调试选项')

    debug_group.add_argument(
        '-v', '--verbose',
        action='store_true',
        help='显示详细输出'
    )

    debug_group.add_argument(
        '--debug',
        action='store_true',
        help='启用调试输出（显示详细的生成过程信息）'
    )

    debug_group.add_argument(
        '--dry-run',
        action='store_true',
        help='只解析不生成文件，用于测试'
    )

    args = parser.parse_args()

    # 验证输出目录参数
    if not args.output_dir and not args.raw_output_dir:
        parser.error("必须指定 --output-dir 或 --raw-output-dir")

    # 如果指定了 --all，启用所有生成选项
    if args.all:
        args.swig = True
        args.cpp_wrapper = True
        args.cpp_implements = True

    # 确定各类型文件的输出目录
    default_output = args.output_dir or args.raw_output_dir
    raw_output_dir = args.raw_output_dir or default_output
    wrapper_output_dir = args.wrapper_output_dir or default_output
    implements_output_dir = args.implements_output_dir or default_output
    swig_output_dir = args.swig_output_dir or default_output

    # 处理多个输入文件
    input_files = [f.strip() for f in args.input.split(',')]
    all_generated_files: List[str] = []
    all_swig_files: List[str] = []

    for input_file in input_files:
        # 验证输入文件
        input_path = Path(input_file)
        if not input_path.exists():
            print(f"错误: 输入文件不存在: {input_path}", file=sys.stderr)
            return 1

        # 确定基础名称
        base_name = args.base_name or input_path.stem

        # 解析 IDL 文件
        try:
            from das_idl_parser import parse_idl_file

            if args.verbose:
                print(f"解析 IDL 文件: {input_path}")

            document = parse_idl_file(str(input_path))

            if args.verbose:
                print(f"  找到 {len(document.enums)} 个枚举")
                print(f"  找到 {len(document.interfaces)} 个接口")
                for enum in document.enums:
                    print(f"    - 枚举: {enum.name} ({len(enum.values)} 个值)")
                for iface in document.interfaces:
                    print(f"    - 接口: {iface.name} : {iface.base_interface}")
                    print(f"        UUID: {iface.uuid}")
                    print(f"        方法: {len(iface.methods)}, 属性: {len(iface.properties)}")

        except SyntaxError as e:
            print(f"语法错误: {e}", file=sys.stderr)
            return 2
        except Exception as e:
            print(f"解析错误: {e}", file=sys.stderr)
            if args.verbose:
                import traceback
                traceback.print_exc()
            return 2

        # 检查是否只是测试运行
        if args.dry_run:
            print(f"Dry run 模式，跳过 {input_path} 的代码生成")
            continue

        # === 生成原始 C++ 代码 ===
        try:
            from das_cpp_generator import generate_cpp_files

            output_dir = Path(raw_output_dir)
            output_dir.mkdir(parents=True, exist_ok=True)

            if args.verbose:
                print(f"生成原始 C++ 代码到: {output_dir}")

            cpp_files = generate_cpp_files(
                document=document,
                output_dir=str(output_dir),
                base_name=base_name,
                namespace=args.namespace,
                idl_file_path=str(input_path)
            )
            all_generated_files.extend(cpp_files)

        except Exception as e:
            print(f"C++ 生成错误: {e}", file=sys.stderr)
            if args.verbose:
                import traceback
                traceback.print_exc()
            return 3

        # === 生成 C++ 包装文件（如果启用）===
        if args.cpp_wrapper:
            try:
                from das_cpp_wrapper_generator import generate_cpp_wrapper_files

                output_dir = Path(wrapper_output_dir)
                output_dir.mkdir(parents=True, exist_ok=True)

                if args.verbose:
                    print(f"生成 C++ 包装文件到: {output_dir}")

                wrapper_files = generate_cpp_wrapper_files(
                    document=document,
                    output_dir=str(output_dir),
                    base_name=base_name,
                    namespace=args.wrapper_namespace,
                    idl_file_path=str(input_path)
                )
                all_generated_files.extend(wrapper_files)

            except Exception as e:
                print(f"C++ 包装文件生成错误: {e}", file=sys.stderr)
                if args.verbose:
                    import traceback
                    traceback.print_exc()
                return 4

        # === 生成 C++ 实现基类模板（如果启用）===
        if args.cpp_implements:
            try:
                from das_cpp_implements_generator import generate_cpp_implements_files

                output_dir = Path(implements_output_dir)
                output_dir.mkdir(parents=True, exist_ok=True)

                if args.verbose:
                    print(f"生成 C++ 实现基类模板到: {output_dir}")

                implements_files = generate_cpp_implements_files(
                    document=document,
                    output_dir=str(output_dir),
                    base_name=base_name,
                    namespace=args.wrapper_namespace,
                    idl_file_path=str(input_path)
                )
                all_generated_files.extend(implements_files)

            except Exception as e:
                print(f"C++ 实现基类模板生成错误: {e}", file=sys.stderr)
                if args.verbose:
                    import traceback
                    traceback.print_exc()
                return 5

        # === 生成 SWIG 代码（如果启用）===
        if args.swig:
            try:
                from das_swig_generator import generate_swig_files

                output_dir = Path(swig_output_dir)
                output_dir.mkdir(parents=True, exist_ok=True)

                if args.verbose:
                    print(f"生成 SWIG .i 文件到: {output_dir}")

                # 如果指定了generate_type_maps，使用文件名作为task_id
                task_id = ""
                output_typemap_info = False
                if args.generate_type_maps:
                    task_id = base_name
                    output_typemap_info = True

                swig_files = generate_swig_files(
                    document=document,
                    output_dir=str(output_dir),
                    base_name=base_name,
                    idl_file_path=str(input_path),
                    output_typemap_info=output_typemap_info,
                    task_id=task_id,
                    debug=args.debug
                )
                all_swig_files.extend(swig_files)
                all_generated_files.extend(swig_files)

            except Exception as e:
                print(f"SWIG 生成错误: {e}", file=sys.stderr)
                if args.verbose:
                    import traceback
                    traceback.print_exc()
                return 6

    # 如果有多个 IDL 文件且启用了 SWIG，生成一个总汇总文件
    if args.swig and len(input_files) > 1 and not args.dry_run:
        try:
            output_dir = Path(swig_output_dir)
            master_i_path = output_dir / "DasGenerated.i"

            # 收集所有单独的汇总 .i 文件
            individual_i_files = [f for f in all_swig_files if f.endswith('.i') and not f.endswith('DasGenerated.i')]
            # 只包含每个 base_name 的汇总文件
            base_i_files = [f for f in individual_i_files if not any(
                iface_name in os.path.basename(f)
                for input_file in input_files
                for iface_name in ['IDas']  # 简化判断
            )]

            with open(master_i_path, 'w', encoding='utf-8') as f:
                f.write("// Master SWIG interface file - includes all generated .i files\n")
                f.write("// !!! DO NOT EDIT !!!\n\n")
                for i_file in sorted(set(os.path.basename(p) for p in all_swig_files if p.endswith('.i'))):
                    # 只包含汇总文件（不包含单个接口的 .i 文件）
                    if not i_file.startswith('IDas'):
                        f.write(f'%include "{i_file}"\n')

            print(f"Generated master: {master_i_path}")
            all_generated_files.append(str(master_i_path))
        except Exception as e:
            print(f"生成总汇总文件错误: {e}", file=sys.stderr)

    # 打印总结
    if not args.dry_run:
        print(f"\n{'='*60}")
        print(f"成功生成 {len(all_generated_files)} 个文件:")
        print(f"{'='*60}")

        # 按目录分组显示
        files_by_dir = {}
        for f in all_generated_files:
            dir_path = os.path.dirname(f)
            if dir_path not in files_by_dir:
                files_by_dir[dir_path] = []
            files_by_dir[dir_path].append(os.path.basename(f))

        for dir_path, files in sorted(files_by_dir.items()):
            print(f"\n[{dir_path}]")
            for f in sorted(files):
                print(f"  - {f}")

    return 0


def generate_type_maps_from_jsons(json_files: List[str], output_path: str, build_dir: Optional[Path] = None) -> None:
    """从typemap_info_*.json文件汇总生成DasTypeMaps.i

    Args:
        json_files: typemap_info JSON文件路径列表
        output_path: 输出DasTypeMaps.i的路径（相对于SOURCE_DIR或build_dir）
        build_dir: 构建目录（可选），如果提供则输出到该目录
    """
    import json
    from pathlib import Path

    SOURCE_DIR = Path(__file__).parent.parent.parent.parent

    output_dir = build_dir if build_dir else SOURCE_DIR

    lines = []
    lines.append("// DasTypeMaps.i - Unified typemap definitions for all interfaces")
    lines.append("// This file is automatically generated by das_idl_gen.py")
    lines.append("// !!! DO NOT EDIT !!!\n")

    try:
        from das_swig_generator import SwigCodeGenerator

        # Collect all typemaps and ret_classes from all typemap_info JSON files
        # These files contain both static and dynamic typemaps from IDL processing
        all_typemaps = {}
        ignore_typemaps = {}
        ret_classes = {}
        header_blocks = {}

        for json_file in json_files:
            try:
                with open(json_file, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                if 'typemaps' in data:
                    for sig, info in data['typemaps'].items():
                        if sig not in all_typemaps:  # Use first occurrence (deduplicate)
                            all_typemaps[sig] = info
                if 'typemaps_ignore' in data:
                    for sig, info in data['typemaps_ignore'].items():
                        if sig not in ignore_typemaps:  # Use first occurrence (deduplicate)
                            ignore_typemaps[sig] = info
                if 'ret_classes' in data:
                    for class_name, info in data['ret_classes'].items():
                        if class_name not in ret_classes:  # Use first occurrence (deduplicate)
                            ret_classes[class_name] = info
                if 'header_blocks' in data:
                    for interface_name, info in data['header_blocks'].items():
                        if interface_name not in header_blocks:  # Use first occurrence (deduplicate)
                            header_blocks[interface_name] = info
            except Exception as e:
                print(f"Warning: Failed to read typemap_info from {json_file}: {e}", file=sys.stderr)

        # %extend typemaps are in all_typemaps, %ignore typemaps are already in ignore_typemaps
        extend_typemaps = all_typemaps

        # Generate DasTypeMapsIgnore.i (to be included BEFORE class definitions)
        ignore_lines = []
        ignore_lines.append("// DasTypeMapsIgnore.i - %ignore definitions (must be included BEFORE class definitions)")
        ignore_lines.append("// This file is automatically generated by das_idl_gen.py")
        ignore_lines.append("// !!! DO NOT EDIT !!!\n")
        ignore_lines.append("// ============================================================================\n")
        ignore_lines.append("// %ignore directives (to hide original methods with [out] parameters)\n")
        ignore_lines.append("// ============================================================================\n")
        
        for sig in sorted(ignore_typemaps.keys()):
            ignore_lines.append(f"// {sig}")
            ignore_lines.append(ignore_typemaps[sig]['code'] if isinstance(ignore_typemaps[sig], dict) else ignore_typemaps[sig])
            ignore_lines.append("")
        
        ignore_file = output_dir / "DasTypeMapsIgnore.i"
        ignore_file.parent.mkdir(parents=True, exist_ok=True)
        with open(ignore_file, 'w', encoding='utf-8') as f:
            f.write("\n".join(ignore_lines))
        print(f"Generated DasTypeMapsIgnore.i: {ignore_file}")
        
        # Generate DasTypeMapsExtend.i (to be included AFTER class definitions)
        extend_lines = []
        extend_lines.append("// DasTypeMapsExtend.i - %extend and DasRetXxx definitions (must be included AFTER class definitions)")
        extend_lines.append("// This file is automatically generated by das_idl_gen.py")
        extend_lines.append("// !!! DO NOT EDIT !!!\n")
        
        # Header blocks must come FIRST (before DasRetXxx definitions)
        # This ensures headers are included before struct definitions in generated C++ code
        extend_lines.append("// ============================================================================\n")
        extend_lines.append("// Header blocks (collected from interface .i files)\n")
        extend_lines.append("// These %{ ... %} blocks contain necessary headers for DasRetXxx classes\n")
        extend_lines.append("// NOTE: Placed at the beginning to ensure headers are included before struct definitions\n")
        extend_lines.append("// ============================================================================\n")

        for interface_name in sorted(header_blocks.keys()):
            extend_lines.append(f"// Header block from {interface_name}")
            extend_lines.append(header_blocks[interface_name]['code'] if isinstance(header_blocks[interface_name], dict) else header_blocks[interface_name])
            extend_lines.append("")
        
        extend_lines.append("// ============================================================================\n")
        extend_lines.append("// DasRetXxx class definitions (return wrappers for [out] parameters)\n")
        extend_lines.append("// ============================================================================\n")

        for class_name in sorted(ret_classes.keys()):
            extend_lines.append(f"// {class_name}")
            extend_lines.append(ret_classes[class_name]['code'] if isinstance(ret_classes[class_name], dict) else ret_classes[class_name])
            extend_lines.append("")

        extend_lines.append("// ============================================================================\n")
        extend_lines.append("// %extend directives (to add wrapper methods returning DasRetXxx)\n")
        extend_lines.append("// ============================================================================\n")

        for sig in sorted(extend_typemaps.keys()):
            extend_lines.append(f"// {sig}")
            extend_lines.append(extend_typemaps[sig]['code'] if isinstance(extend_typemaps[sig], dict) else extend_typemaps[sig])
            extend_lines.append("")
        
        extend_file = output_dir / "DasTypeMapsExtend.i"
        extend_file.parent.mkdir(parents=True, exist_ok=True)
        with open(extend_file, 'w', encoding='utf-8') as f:
            f.write("\n".join(extend_lines))
        print(f"Generated DasTypeMapsExtend.i: {extend_file}")
        
        # Also generate the original DasTypeMaps.i for backward compatibility
        # (it just includes both files in the correct order)
        lines = []
        lines.append("// DasTypeMaps.i - Unified typemap definitions for all interfaces")
        lines.append("// This file is automatically generated by das_idl_gen.py")
        lines.append("// !!! DO NOT EDIT !!!")
        lines.append("// NOTE: This file now includes DasTypeMapsIgnore.i and DasTypeMapsExtend.i")
        lines.append("// For proper ordering, include DasTypeMapsIgnore.i BEFORE class definitions")
        lines.append("// and DasTypeMapsExtend.i AFTER class definitions\n")
        lines.append("%include \"DasTypeMapsIgnore.i\"")
        lines.append("%include \"DasTypeMapsExtend.i\"")

    except Exception as e:
        print(f"Warning: Failed to read typemap_info JSON files: {e}", file=sys.stderr)

    output_file = output_dir / output_path
    output_file.parent.mkdir(parents=True, exist_ok=True)

    with open(output_file, 'w', encoding='utf-8') as f:
        f.write("\n".join(lines))

    print(f"Generated DasTypeMaps.i: {output_file}")


if __name__ == '__main__':
    sys.exit(main())
