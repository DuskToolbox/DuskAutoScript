#!/usr/bin/env python3
"""Generate Node/NAPI aggregate export files from IDL definitions."""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path
from typing import Sequence

sys.path.insert(0, str(Path(__file__).parent))

from das_idl_parser import IdlDocument, parse_idl_file
from napi_generator import generate_napi_artifacts
from shared_utils import idl_path_to_header_name

NODE_HOST_SCRIPT_NAME = "das-node-host.cjs"
NODE_HOST_SCRIPT_SOURCE = Path(__file__).parent / "node_host" / NODE_HOST_SCRIPT_NAME
NODE_PACKAGE_HOST_SCRIPT = f"bin/{NODE_HOST_SCRIPT_NAME}"


def _node_package_json(package_name: str) -> str:
    return (
        json.dumps(
            {
                "name": package_name,
                "version": "0.0.0",
                "private": True,
                "type": "commonjs",
                "main": "index.cjs",
                "types": "index.d.ts",
                "bin": {
                    "das-node-host": NODE_PACKAGE_HOST_SCRIPT,
                },
            },
            indent=2,
        )
        + "\n"
    )


def _node_index_cjs(addon_name: str) -> str:
    return f"'use strict';\n\nmodule.exports = require(\"./{addon_name}_export.js\");\n"


def _node_index_dts(addon_name: str) -> str:
    return f'export * from "./{addon_name}_export";\n'


def _merge_documents(documents: Sequence[IdlDocument]) -> IdlDocument:
    merged = IdlDocument()
    seen_interfaces: set[str] = set()
    seen_enums: set[str] = set()
    seen_error_codes: set[str] = set()

    for doc in documents:
        for interface in doc.interfaces:
            key = f"{interface.namespace}::{interface.name}"
            if key not in seen_interfaces:
                merged.interfaces.append(interface)
                seen_interfaces.add(key)

        for enum in doc.enums:
            key = f"{enum.namespace}::{enum.name}"
            if key not in seen_enums:
                merged.enums.append(enum)
                seen_enums.add(key)

        for struct in doc.structs:
            merged.structs.append(struct)

        for error_code in doc.error_codes:
            key = f"{error_code.namespace}::{error_code.name}"
            if key not in seen_error_codes:
                merged.error_codes.append(error_code)
                seen_error_codes.add(key)

        for module in doc.modules:
            merged.modules.append(module)

    return merged


def _validate_nonempty(value: str, display_name: str) -> str:
    if not value or not value.strip():
        raise ValueError(f"{display_name} must not be empty")
    return value.strip()


def _validate_addon_name(addon_name: str) -> str:
    value = _validate_nonempty(addon_name, "--addon-name")
    candidate = Path(value)
    if candidate.name != value or any(part == ".." for part in candidate.parts):
        raise ValueError("--addon-name must be a file basename, not a path")
    return value


def _resolve_output_path(output_dir: Path, file_name: str) -> Path:
    relative = Path(file_name)
    if relative.is_absolute() or any(part == ".." for part in relative.parts):
        raise ValueError(f"refusing to write outside output directory: {file_name}")
    output_root = output_dir.resolve()
    target = (output_dir / relative).resolve()
    try:
        target.relative_to(output_root)
    except ValueError as exc:
        raise ValueError(f"refusing to write outside output directory: {target}")
    return target


def _write_output(output_dir: Path, file_name: str, text: str) -> None:
    target = _resolve_output_path(output_dir, file_name)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8")
    print(f"Generated: {target}")


def _copy_output(output_dir: Path, source: Path, file_name: str) -> None:
    target = _resolve_output_path(output_dir, file_name)
    if not source.exists():
        raise FileNotFoundError(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    print(f"Generated: {target}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate Node/NAPI aggregate export files from DAS IDL definitions",
    )
    parser.add_argument(
        "--idl-dir",
        required=True,
        help="Directory containing IDL source files",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output directory for generated Node/NAPI support files",
    )
    parser.add_argument(
        "--package-name",
        required=True,
        help="Public JavaScript package identity, for example das-core",
    )
    parser.add_argument(
        "--addon-name",
        required=True,
        help="Native addon basename, for example das_core_napi",
    )
    parser.add_argument(
        "--idl-files",
        nargs="+",
        required=True,
        help="List of IDL file names relative to --idl-dir",
    )
    args = parser.parse_args(argv)

    try:
        package_name = _validate_nonempty(args.package_name, "--package-name")
        addon_name = _validate_addon_name(args.addon_name)
    except ValueError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    idl_dir = Path(args.idl_dir)
    documents: list[IdlDocument] = []
    for idl_file in args.idl_files:
        path = idl_dir / idl_file
        if not path.exists():
            print(f"Error: IDL file not found: {path}", file=sys.stderr)
            return 1
        try:
            documents.append(parse_idl_file(str(path)))
        except Exception as exc:
            print(f"Error parsing {path}: {exc}", file=sys.stderr)
            return 2

    if not documents:
        print("Error: no IDL files parsed successfully", file=sys.stderr)
        return 3

    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    merged = _merge_documents(documents)
    artifacts = generate_napi_artifacts(
        merged,
        package_name=package_name,
        addon_name=addon_name,
        idl_header_names=[idl_path_to_header_name(name) for name in args.idl_files],
    )

    try:
        _write_output(output_dir, f"{addon_name}_export.cpp", artifacts.cpp)
        _write_output(output_dir, f"{addon_name}_export.d.ts", artifacts.dts)
        _write_output(output_dir, f"{addon_name}_export.js", artifacts.js)
        _write_output(output_dir, "package.json", _node_package_json(package_name))
        _write_output(output_dir, "index.cjs", _node_index_cjs(addon_name))
        _write_output(output_dir, "index.d.ts", _node_index_dts(addon_name))
        _copy_output(output_dir, NODE_HOST_SCRIPT_SOURCE, NODE_PACKAGE_HOST_SCRIPT)
    except OSError as exc:
        print(f"Error writing NAPI output: {exc}", file=sys.stderr)
        return 4
    except ValueError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 4

    return 0


if __name__ == "__main__":
    sys.exit(main())
