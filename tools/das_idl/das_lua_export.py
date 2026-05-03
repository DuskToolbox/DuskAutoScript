#!/usr/bin/env python3
"""Generate sol2 Lua export files from IDL definitions.

This script is called from the CMake build system (DasAddIdlExport.cmake)
when EXPORT_LUA=ON. It parses IDL files and produces:
  - {NAME}_lua_export.cpp  — sol2 C++ binding code (compiled into a SHARED library)
  - {NAME}_lua_export.lua   — EmmyLua type stubs for LuaLS

Usage:
    python das_lua_export.py \\
        --idl-dir /path/to/idl \\
        --output /path/to/output \\
        --name DasCore \\
        --idl-files IDasBase.idl IDasLogger.idl ...
"""

import argparse
import os
import sys
from pathlib import Path
from typing import List

# Ensure the tools/das_idl directory is on sys.path so we can import sibling modules
sys.path.insert(0, str(Path(__file__).parent))

from das_idl_parser import parse_idl_file, IdlDocument, InterfaceDef
from swig_lua_generator import LuaSwigGenerator


def _merge_documents(documents: List[IdlDocument]) -> IdlDocument:
    """Merge multiple IdlDocument objects into a single document.

    Collects all interfaces, enums, structs, error codes, and modules
    from the individual per-file documents into one combined document.

    Args:
        documents: List of IdlDocument objects parsed from individual IDL files.

    Returns:
        A merged IdlDocument containing all definitions.
    """
    merged = IdlDocument()
    seen_interface_names: set = set()
    seen_enum_names: set = set()
    seen_module_names: set = set()

    for doc in documents:
        for iface in doc.interfaces:
            if iface.name not in seen_interface_names:
                merged.interfaces.append(iface)
                seen_interface_names.add(iface.name)

        for enum in doc.enums:
            if enum.name not in seen_enum_names:
                merged.enums.append(enum)
                seen_enum_names.add(enum.name)

        for struct in doc.structs:
            merged.structs.append(struct)

        for ec in doc.error_codes:
            merged.error_codes.append(ec)

        for mod in doc.modules:
            if mod.module_name not in seen_module_names:
                merged.modules.append(mod)
                seen_module_names.add(mod.module_name)

    return merged


def generate_cpp_file(
    gen: LuaSwigGenerator,
    doc: IdlDocument,
    interfaces: List[InterfaceDef],
    name: str,
) -> str:
    """Generate the complete C++ source file for sol2 Lua bindings.

    The generated file contains:
      1. #include directives (sol2, DAS core headers)
      2. LuaDirector base class
      3. Director wrapper classes for each interface
      4. Registration functions (interfaces, error codes, modules)
      5. luaopen entry point

    Args:
        gen: LuaSwigGenerator instance.
        doc: Merged IdlDocument with resolved types.
        interfaces: List of InterfaceDef objects (from doc.interfaces).
        name: Export library base name (e.g., "DasCore").

    Returns:
        Complete C++ source code as a string.
    """
    parts: List[str] = []

    # ── Include directives ─────────────────────────────────────────────
    parts.append('#include <sol/sol.hpp>')
    parts.append('')

    # Include ABI headers for each IDL file
    for iface in interfaces:
        parts.append(f'#include "das/_autogen/idl/abi/{iface.name}.h"')
    parts.append('')

    # Include wrapper headers (for convenience types)
    parts.append('#include "das/DasCore.h"')
    parts.append('')

    # ── LuaDirector base class ─────────────────────────────────────────
    parts.append(gen._generate_lua_director_base())
    parts.append('')

    # ── Director wrapper classes ───────────────────────────────────────
    for interface in interfaces:
        parts.append(gen._generate_director_class(interface))
        parts.append('')

    # ── Registration function (interfaces + directors) ─────────────────
    parts.append(gen._generate_registration_function(interfaces))
    parts.append('')

    # ── Error code bindings ────────────────────────────────────────────
    if doc.error_codes:
        parts.append(gen._generate_errorcode_binding(doc))
        parts.append('')

    # ── Module function bindings ───────────────────────────────────────
    has_functions = any(len(module.functions) > 0 for module in doc.modules)
    if has_functions:
        parts.append(gen._generate_module_binding(doc))
        parts.append('')

    # ── luaopen entry point ────────────────────────────────────────────
    parts.append(gen._generate_luaopen_function(doc, interfaces))

    return '\n'.join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(
        description='Generate sol2 Lua export files from DAS IDL definitions',
    )
    parser.add_argument(
        '--idl-dir',
        required=True,
        help='Directory containing IDL source files',
    )
    parser.add_argument(
        '--output',
        required=True,
        help='Output directory for generated files',
    )
    parser.add_argument(
        '--name',
        default='DasCore',
        help='Base name for generated files (default: DasCore)',
    )
    parser.add_argument(
        '--idl-files',
        nargs='+',
        required=True,
        help='List of IDL file names (relative to --idl-dir)',
    )
    args = parser.parse_args()

    # ── Parse all IDL files ────────────────────────────────────────────
    documents: List[IdlDocument] = []
    for idl_file in args.idl_files:
        path = os.path.join(args.idl_dir, idl_file)
        if not os.path.exists(path):
            print(f'Error: IDL file not found: {path}', file=sys.stderr)
            return 1
        try:
            doc = parse_idl_file(path)
            documents.append(doc)
        except Exception as e:
            print(f'Error parsing {path}: {e}', file=sys.stderr)
            return 2

    if not documents:
        print('Error: no IDL files parsed successfully', file=sys.stderr)
        return 3

    # ── Merge documents ────────────────────────────────────────────────
    merged_doc = _merge_documents(documents)

    # ── Generate ───────────────────────────────────────────────────────
    gen = LuaSwigGenerator()
    interfaces = merged_doc.interfaces

    # Generate C++ source
    cpp_code = generate_cpp_file(gen, merged_doc, interfaces, args.name)

    # Generate Lua stub
    lua_stub = gen._generate_lua_stub(interfaces, merged_doc)

    # ── Write output files ─────────────────────────────────────────────
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    cpp_path = output_dir / f'{args.name}_lua_export.cpp'
    lua_path = output_dir / f'{args.name}_lua_export.lua'

    cpp_path.write_text(cpp_code, encoding='utf-8')
    lua_path.write_text(lua_stub, encoding='utf-8')

    print(f'Generated: {cpp_path}')
    print(f'Generated: {lua_path}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
