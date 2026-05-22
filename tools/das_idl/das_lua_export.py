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
        --open-module-name DasCoreApi \\
        --idl-files IDasBase.idl IDasLogger.idl ...
"""

import argparse
import os
import re
import sys
from pathlib import Path
from typing import List

# Ensure the tools/das_idl directory is on sys.path so we can import sibling modules
sys.path.insert(0, str(Path(__file__).parent))

from das_idl_parser import parse_idl_file, IdlDocument, InterfaceDef
from swig_lua_generator import LuaSwigGenerator


def _collect_abi_interface_names(abi_dir: str) -> set[str]:
    """Collect interface names that are available from generated ABI headers."""
    available: set[str] = set()
    abi_path = Path(abi_dir)
    if not abi_path.is_dir():
        return available

    for header in abi_path.glob('*.h'):
        try:
            text = header.read_text(encoding='utf-8')
        except UnicodeDecodeError:
            text = header.read_text(encoding='utf-8', errors='ignore')
        available.update(
            re.findall(r'\bDAS_INTERFACE\s+([A-Za-z_][A-Za-z0-9_]*)\b', text)
        )
    return available


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
            merged.modules.append(mod)

    return merged


def generate_cpp_file(
    gen: LuaSwigGenerator,
    doc: IdlDocument,
    interfaces: List[InterfaceDef],
    name: str,
    open_module_name: str,
    abi_dir: str,
    export_c_macro: str,
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
        open_module_name: Module suffix for the exported luaopen_<name> symbol.
        abi_dir: Path to the ABI header directory for checking existence.
        export_c_macro: Export macro for luaopen entry point (e.g. "DAS_C_API").

    Returns:
        Complete C++ source code as a string.
    """
    parts: List[str] = []

    # ── Warning suppression (MUST be before #include <sol/sol.hpp>) ──
    # 抑制 sol2 为抽象 COM 接口注册时触发的 -Wdelete-abstract-non-virtual-dtor 警告。
    # sol2 的 new_usertype<T>() 内部无条件实例化 make_destructor<T>()，
    # 但抽象接口使用 sol::no_constructor 且 GC 已重定向到 Release()，
    # 运行时不会调用析构函数。
    # 需要放在 #include <sol/sol.hpp> 之前，确保 pragma 覆盖 sol2 模板实例化。
    parts.append('#ifdef __clang__')
    parts.append('#pragma clang diagnostic push')
    parts.append(
        '#pragma clang diagnostic ignored'
        ' "-Wdelete-abstract-non-virtual-dtor"'
    )
    parts.append('#elif defined(__GNUC__)')
    parts.append('#pragma GCC diagnostic push')
    parts.append(
        '#pragma GCC diagnostic ignored'
        ' "-Wdelete-abstract-non-virtual-dtor"'
    )
    parts.append('#elif defined(_MSC_VER)')
    parts.append('#pragma warning(push)')
    parts.append('#pragma warning(disable: 4265)')
    parts.append('#endif')
    parts.append('')

    # ── Include directives ─────────────────────────────────────────────
    # Force include our bundled Lua headers BEFORE sol2 picks up system Lua.
    # This prevents msys2's Lua 5.5 headers (from internal-isystem) from being
    # found first, which would break sol2's version detection.
    # We must include ALL Lua headers that share header guards across versions
    # (lua_h, lauxlib_h, lualib_h are identical in both Lua 5.4 and 5.5),
    # otherwise msys2's Lua 5.5 lualib.h will redefine luaL_openlibs as a macro
    # that expands to luaL_openselectedlibs (a Lua 5.5-only symbol).
    # Also wrap in extern "C" because the Lua GitHub repo's lua.h lacks the
    # extern "C" block that the official Lua .tar.gz release includes.
    parts.append('extern "C" {')
    parts.append('#include "lua.h"')
    parts.append('#include "lauxlib.h"')
    parts.append('#include "lualib.h"')
    parts.append('}')
    parts.append('#include <cstdio>')
    parts.append('#include <string>')
    parts.append('#include <sol/sol.hpp>')
    parts.append('')

    # Include all ABI headers because one IDL file can define multiple
    # interfaces while generating a single header named after the file.
    for header in sorted(Path(abi_dir).glob('*.h')):
        parts.append(f'#include "das/_autogen/idl/abi/{header.name}"')
    parts.append('')

    # Include wrapper headers (for convenience types)
    parts.append('#include "das/DasApi.h"')
    parts.append('#include "das/DasSwigApi.h"')
    # Include additional headers for types used in module functions
    # but not covered by individual ABI headers
    parts.append('#include "das/IDasCoreServices.h"')
    # IDasBase is needed by ExtractDasBasePointer() and Director base classes
    parts.append('#include "das/IDasBase.h"')
    parts.append('')

    # Bring interface namespaces into scope so that generated code can use
    # interface names (IDasBinaryBuffer, IDasCapture, etc.) without
    # full qualification.  ABI headers define interfaces inside
    # Das::ExportInterface or Das::PluginInterface.
    parts.append('using namespace Das::ExportInterface;')
    parts.append('using namespace Das::PluginInterface;')
    parts.append('')

    # Filter interfaces to only those available from ABI headers. Do not require
    # a same-named header: multi-interface IDL files such as IDasComponent.idl
    # generate IDasComponent.h containing both IDasComponent and
    # IDasComponentFactory.
    available_interface_names = _collect_abi_interface_names(abi_dir)
    abilable_interfaces = [
        iface for iface in interfaces if iface.name in available_interface_names
    ]

    # ── LuaDirector base class ─────────────────────────────────────────
    parts.append(gen._generate_lua_director_base())
    parts.append('')

    # ── Director wrapper classes ───────────────────────────────────────
    # Build interface name → InterfaceDef map for inherited method resolution
    iface_map = {iface.name: iface for iface in abilable_interfaces}
    for interface in abilable_interfaces:
        parts.append(gen._generate_director_class(interface, iface_map))
        parts.append('')

    # ── ExtractDasBasePointer helper ───────────────────────────────────
    # 导出辅助函数，供 DasCore.dll 跨 DLL 从 Lua userdata 提取 IDasBase* 指针。
    # 同时供 IDasVariantVector::PushBackBase/SetBase 绑定修正 Director→IDasBase
    # 的多继承指针调整。
    parts.append(gen._generate_extract_base_pointer_function(export_c_macro, abilable_interfaces))
    parts.append('')

    # ── Registration function (interfaces + directors) ─────────────────
    parts.append(gen._generate_registration_function(abilable_interfaces))
    parts.append('')

    # ── Error code bindings ────────────────────────────────────────────
    if doc.error_codes:
        parts.append(gen._generate_errorcode_binding(doc))
        parts.append('')

    # ── Module function bindings ───────────────────────────────────────
    has_functions = any(len(module.functions) > 0 for module in doc.modules)
    if has_functions:
        available_types = {iface.name for iface in abilable_interfaces}
        parts.append(
            gen._generate_module_binding(doc, available_types)
        )
        parts.append('')

    # ── luaopen entry point ────────────────────────────────────────────
    parts.append(
        gen._generate_luaopen_function(
            doc,
            abilable_interfaces,
            export_c_macro,
            open_module_name,
        )
    )
    parts.append('')

    # ── Restore warning state ──────────────────────────────────────────
    parts.append('#ifdef __clang__')
    parts.append('#pragma clang diagnostic pop')
    parts.append('#elif defined(__GNUC__)')
    parts.append('#pragma GCC diagnostic pop')
    parts.append('#elif defined(_MSC_VER)')
    parts.append('#pragma warning(pop)')
    parts.append('#endif')

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
        '--open-module-name',
        required=True,
        help='Module suffix for generated luaopen_<name> symbol',
    )
    parser.add_argument(
        '--idl-files',
        nargs='+',
        required=True,
        help='List of IDL file names (relative to --idl-dir)',
    )
    parser.add_argument(
        '--export-c-macro',
        required=True,
        help='Export macro for luaopen entry point (e.g. DAS_C_API)',
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

    # Determine ABI header directory for include filtering.
    # The ABI headers live at <output>/../abi/ relative to the Lua output dir,
    # since both are under _autogen/idl/ (lua/ and abi/).
    abi_dir = os.path.normpath(os.path.join(args.output, '..', 'abi'))

    # Generate C++ source
    cpp_code = generate_cpp_file(
        gen,
        merged_doc,
        interfaces,
        args.name,
        args.open_module_name,
        abi_dir,
        args.export_c_macro,
    )

    # Filter interfaces for Lua stub — only those available from ABI headers.
    available_interface_names = _collect_abi_interface_names(abi_dir)
    abilable_interfaces = [
        iface for iface in interfaces if iface.name in available_interface_names
    ]

    # Generate Lua stub
    lua_stub = gen._generate_lua_stub(abilable_interfaces, merged_doc)

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
