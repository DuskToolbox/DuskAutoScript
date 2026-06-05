#!/usr/bin/env python3
"""Generate C# aggregate export files from IDL definitions."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Sequence

sys.path.insert(0, str(Path(__file__).parent))

from csharp_generator import generate_csharp_artifacts
from das_idl_parser import IdlDocument, parse_idl_file
from shared_utils import idl_path_to_header_name


def _merge_documents(documents: Sequence[IdlDocument]) -> IdlDocument:
    merged = IdlDocument()
    seen_interfaces: set[str] = set()
    seen_enums: set[str] = set()
    seen_structs: set[str] = set()
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
            key = f"{struct.namespace}::{struct.name}"
            if key not in seen_structs:
                merged.structs.append(struct)
                seen_structs.add(key)

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


def _resolve_output_path(output_dir: Path, file_name: str) -> Path:
    relative = Path(file_name)
    if relative.is_absolute() or any(part == ".." for part in relative.parts):
        raise ValueError(f"refusing to write outside output directory: {file_name}")
    output_root = output_dir.resolve()
    target = (output_dir / relative).resolve()
    try:
        target.relative_to(output_root)
    except ValueError as exc:
        raise ValueError(f"refusing to write outside output directory: {target}") from exc
    return target


def _write_output(output_dir: Path, file_name: str, text: str) -> None:
    target = _resolve_output_path(output_dir, file_name)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8", newline="\n")
    print(f"Generated: {target}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate C# aggregate export files from DAS IDL definitions",
    )
    parser.add_argument("--idl-dir", required=True, help="Directory containing IDL files")
    parser.add_argument("--output", required=True, help="Output directory")
    parser.add_argument("--namespace-root", required=True, help="C# namespace root")
    parser.add_argument("--package-name", required=True, help="C# package identity")
    parser.add_argument("--project-name", required=True, help="C# project basename")
    parser.add_argument(
        "--idl-files",
        nargs="+",
        required=True,
        help="IDL file names relative to --idl-dir",
    )
    args = parser.parse_args(argv)

    try:
        namespace_root = _validate_nonempty(args.namespace_root, "--namespace-root")
        package_name = _validate_nonempty(args.package_name, "--package-name")
        project_name = _validate_nonempty(args.project_name, "--project-name")
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
    artifacts = generate_csharp_artifacts(
        _merge_documents(documents),
        namespace_root=namespace_root,
        package_name=package_name,
        project_name=project_name,
        idl_header_names=[idl_path_to_header_name(name) for name in args.idl_files],
    )

    try:
        for file_name, text in artifacts.files.items():
            _write_output(output_dir, file_name, text)
    except OSError as exc:
        print(f"Error writing C# output: {exc}", file=sys.stderr)
        return 4
    except ValueError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 4

    return 0


if __name__ == "__main__":
    sys.exit(main())
