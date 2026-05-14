"""
DAS IDL 代码生成器包

用于从 IDL 文件生成 C++ 接口代码
"""

import sys
from pathlib import Path

_PACKAGE_DIR = Path(__file__).resolve().parent
if str(_PACKAGE_DIR) not in sys.path:
    sys.path.insert(0, str(_PACKAGE_DIR))

from .das_idl_parser import (
    parse_idl,
    parse_idl_file,
    IdlDocument,
    InterfaceDef,
    EnumDef,
    MethodDef,
    PropertyDef,
    ParameterDef,
    TypeInfo,
    ParamDirection,
    TypeKind,
)

from .das_cpp_generator import (
    CppCodeGenerator,
    CppTypeMapper,
    generate_cpp_files,
)

__version__ = "1.0.0"
__all__ = [
    # Parser
    "parse_idl",
    "parse_idl_file",
    "IdlDocument",
    "InterfaceDef",
    "EnumDef",
    "MethodDef",
    "PropertyDef",
    "ParameterDef",
    "TypeInfo",
    "ParamDirection",
    "TypeKind",
    # Generator
    "CppCodeGenerator",
    "CppTypeMapper",
    "generate_cpp_files",
]
