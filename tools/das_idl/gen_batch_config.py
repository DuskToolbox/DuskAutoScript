"""
gen_batch_config.py - 从 CMake 参数生成 batch config JSON

替代 cmake 的 file(CONFIGURE OUTPUT ...) 机制，用 Python 可靠地生成 JSON 文件。
仅内容变化时才写入，避免更新时间戳触发不必要的重建。

JSON 结构:
{
    "tasks": [
        { "-i": "...", "--raw-output-dir": "...", ... },  // map 阶段参数
        ...
    ],
    "reduce": {                                          // reduce 阶段全局配置
        "lua_output_dir": "...",
        "lua_name": "...",
        "lua_idl_dir": "...",
        "lua_idl_files": [...]
    }
}

用法:
    python gen_batch_config.py \
        --output <json_path> \
        --idl-dir <dir> \
        --idl-files <file1> <file2> ... \
        --raw-output-dir <dir> \
        --wrapper-output-dir <dir> \
        --header-output-dir <dir> \
        --export-macro <macro> \
        --export-c-macro <macro> \
        [--swig-output-dir <dir>] \
        [--ipc-output-dir <dir>] \
        [--ipc-cache-dir <dir>] \
        [--ipc-proxy] \
        [--ipc-stub] \
        [--namespace <ns>] \
        [--languages Python Java Lua ...] \
        [--lua-output-dir <dir>] \
        [--lua-name <name>] \
        [--lua-open-module-name <name>] \
        [--node-output-dir <dir>] \
        [--node-package-name <name>] \
        [--node-addon-name <name>] \
        [--csharp-output-dir <dir>] \
        [--csharp-namespace-root <name>] \
        [--csharp-das-native-module-name <name>] \
        [--csharp-native-support-module-name <name>]
"""

import argparse
import json
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate batch config JSON for das_idl_batch_gen.py",
    )

    # 输出
    parser.add_argument("--output", required=True, help="Output JSON file path")

    # IDL 输入
    parser.add_argument("--idl-dir", required=True, help="IDL source directory")
    parser.add_argument(
        "--idl-files",
        nargs="+",
        required=True,
        help="IDL file names (relative to --idl-dir)",
    )

    # 通用输出目录
    parser.add_argument("--raw-output-dir", required=True, help="ABI output directory")
    parser.add_argument(
        "--wrapper-output-dir", required=True, help="Wrapper output directory"
    )
    parser.add_argument(
        "--header-output-dir", required=True, help="Header output directory"
    )

    # 宏
    parser.add_argument("--export-macro", default="", help="Export macro (e.g. DAS_API)")
    parser.add_argument(
        "--export-c-macro", default="", help="C export macro (e.g. DAS_C_API)"
    )

    # SWIG (可选)
    parser.add_argument("--swig-output-dir", default="", help="SWIG output directory")

    # IPC (可选)
    parser.add_argument("--ipc-output-dir", default="", help="IPC output directory")
    parser.add_argument("--ipc-cache-dir", default="", help="IPC cache directory")
    parser.add_argument(
        "--ipc-proxy", action="store_true", help="Enable IPC proxy generation"
    )
    parser.add_argument(
        "--ipc-stub", action="store_true", help="Enable IPC stub generation"
    )

    # 命名空间 (可选)
    parser.add_argument("--namespace", default="", help="C++ namespace")

    # 语言列表 (可选，用于判断是否需要 SWIG 和 Lua)
    parser.add_argument(
        "--languages",
        nargs="*",
        default=[],
        help="Target languages (Python, Java, CSharp, Lua, Node)",
    )

    # Lua (可选，仅在 languages 包含 Lua 时由 cmake 传入)
    parser.add_argument("--lua-output-dir", default="", help="Lua output directory")
    parser.add_argument("--lua-name", default="", help="Lua module name")
    parser.add_argument(
        "--lua-open-module-name",
        default="",
        help="Lua C API open module name for luaopen_<name>",
    )

    # Node/NAPI (optional, non-SWIG reduce stage)
    parser.add_argument("--node-output-dir", default="", help="Node/NAPI output directory")
    parser.add_argument(
        "--node-package-name",
        default="",
        help="Public JavaScript package identity, for example das-core",
    )
    parser.add_argument(
        "--node-addon-name",
        default="",
        help="Native addon basename, for example das_core_napi",
    )
    parser.add_argument(
        "--csharp-output-dir",
        default="",
        help="C# reduce-stage package output directory",
    )
    parser.add_argument(
        "--csharp-namespace-root",
        default="",
        help="C# generated namespace root, for example Das.Generated",
    )
    parser.add_argument(
        "--csharp-das-native-module-name",
        default="",
        help="Public DAS native module name for generated C# P/Invoke",
    )
    parser.add_argument(
        "--csharp-native-support-module-name",
        default="",
        help="C# native support module name for generated C# helper P/Invoke",
    )

    args = parser.parse_args()

    # 判断是否需要 SWIG (只有 SWIG-backed 语言需要；Lua/Node/CSharp 是 reduce-only)
    swig_languages = {"python", "java"}
    _need_swig = args.swig_output_dir and any(
        lang.lower() in swig_languages for lang in args.languages
    )

    # 构建 per-IDL task 配置（仅包含 map 阶段参数）
    tasks = []
    idl_dir = Path(args.idl_dir)

    for idl_file in args.idl_files:
        task = {
            "-i": str(idl_dir / idl_file),
            "--raw-output-dir": args.raw_output_dir,
            "--wrapper-output-dir": args.wrapper_output_dir,
            "--implements-output-dir": args.wrapper_output_dir,
        }

        # SWIG
        if _need_swig:
            task["--swig-output-dir"] = args.swig_output_dir
            task["--swig"] = True

        task["--cpp-wrapper"] = True
        task["--cpp-implements"] = True

        # IPC
        if args.ipc_proxy or args.ipc_stub:
            task["--ipc-output-dir"] = args.ipc_output_dir
            if args.ipc_cache_dir:
                task["--ipc-cache-dir"] = args.ipc_cache_dir
            if args.ipc_proxy:
                task["--ipc-proxy"] = True
            if args.ipc_stub:
                task["--ipc-stub"] = True

        # 命名空间
        if args.namespace:
            task["--namespace"] = args.namespace

        # Header (所有 task 都有)
        task["--header"] = True
        task["--header-output-dir"] = args.header_output_dir
        if args.export_macro:
            task["--export-macro"] = args.export_macro
        if args.export_c_macro:
            task["--export-c-macro"] = args.export_c_macro

        # Type maps
        task["--generate-type-maps"] = True

        tasks.append(task)

    # 构建 reduce 阶段全局配置
    reduce_config = {}
    has_lua = any(lang.lower() == "lua" for lang in args.languages)
    lua_reduce_requested = has_lua or any(
        [args.lua_output_dir, args.lua_name, args.lua_open_module_name]
    )
    if lua_reduce_requested:
        missing_lua_args = [
            name
            for name, value in [
                ("--lua-output-dir", args.lua_output_dir),
                ("--lua-name", args.lua_name),
                ("--lua-open-module-name", args.lua_open_module_name),
            ]
            if not value
        ]
        if missing_lua_args:
            print(
                "error: Lua reduce config requires "
                + ", ".join(missing_lua_args),
                file=sys.stderr,
            )
            return 2

        reduce_config["lua_output_dir"] = args.lua_output_dir
        reduce_config["lua_name"] = args.lua_name
        reduce_config["lua_open_module_name"] = args.lua_open_module_name
        reduce_config["lua_idl_dir"] = args.idl_dir
        reduce_config["lua_idl_files"] = args.idl_files

    has_node = any(lang.lower() == "node" for lang in args.languages)
    node_reduce_requested = has_node or any(
        [args.node_output_dir, args.node_package_name, args.node_addon_name]
    )
    if node_reduce_requested:
        missing_node_args = [
            name
            for name, value in [
                ("--node-output-dir", args.node_output_dir),
                ("--node-package-name", args.node_package_name),
                ("--node-addon-name", args.node_addon_name),
            ]
            if not value
        ]
        if missing_node_args:
            print(
                "error: Node reduce config requires "
                + ", ".join(missing_node_args),
                file=sys.stderr,
            )
            return 2

        reduce_config["node_output_dir"] = args.node_output_dir
        reduce_config["node_package_name"] = args.node_package_name
        reduce_config["node_addon_name"] = args.node_addon_name
        reduce_config["node_idl_dir"] = args.idl_dir
        reduce_config["node_idl_files"] = args.idl_files

    has_csharp = any(lang.lower() == "csharp" for lang in args.languages)
    csharp_reduce_requested = has_csharp or any(
        [
            args.csharp_output_dir,
            args.csharp_namespace_root,
            args.csharp_das_native_module_name,
            args.csharp_native_support_module_name,
        ]
    )
    if csharp_reduce_requested:
        missing_csharp_args = [
            name
            for name, value in [
                ("--csharp-output-dir", args.csharp_output_dir),
                ("--csharp-namespace-root", args.csharp_namespace_root),
                (
                    "--csharp-das-native-module-name",
                    args.csharp_das_native_module_name,
                ),
                (
                    "--csharp-native-support-module-name",
                    args.csharp_native_support_module_name,
                ),
            ]
            if not value
        ]
        if missing_csharp_args:
            print(
                "error: C# reduce config requires "
                + ", ".join(missing_csharp_args),
                file=sys.stderr,
            )
            return 2

        if not (
            args.csharp_namespace_root == "Das"
            or args.csharp_namespace_root.startswith("Das.")
        ):
            print(
                "error: C# namespace root must be Das or start with Das.",
                file=sys.stderr,
            )
            return 2

        reduce_config["csharp_output_dir"] = args.csharp_output_dir
        reduce_config["csharp_namespace_root"] = args.csharp_namespace_root
        reduce_config["csharp_das_native_module_name"] = (
            args.csharp_das_native_module_name
        )
        reduce_config["csharp_native_support_module_name"] = (
            args.csharp_native_support_module_name
        )
        reduce_config["csharp_idl_dir"] = args.idl_dir
        reduce_config["csharp_idl_files"] = args.idl_files

    # 组装最终 JSON
    config = {"tasks": tasks}
    if reduce_config:
        config["reduce"] = reduce_config

    # 仅内容变化时才写入，避免更新时间戳触发不必要的重建
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    new_content = json.dumps(config, indent=4, ensure_ascii=False) + "\n"
    old_content = (
        output_path.read_text(encoding="utf-8") if output_path.exists() else ""
    )

    if new_content != old_content:
        with open(output_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(new_content)

    return 0


if __name__ == "__main__":
    sys.exit(main())
