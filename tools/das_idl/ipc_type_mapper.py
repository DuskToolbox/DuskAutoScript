"""IPC TypeMapper shared base class and subclasses.

Consolidates the near-identical type mapping logic used by
das_ipc_proxy_generator (ProxyTypeMapper) and
das_ipc_stub_generator (StubTypeMapper).
"""

import os
import re
from pathlib import Path
from typing import Dict, List, Optional

import importlib
import sys

# ---------------------------------------------------------------------------
# Parser type imports — support both package import and direct script execution
# ---------------------------------------------------------------------------

try:
    from . import das_idl_parser as _das_idl_parser
except ImportError:
    this_dir = str(Path(__file__).resolve().parent)
    if this_dir not in sys.path:
        sys.path.insert(0, this_dir)
    _das_idl_parser = importlib.import_module("das_idl_parser")

IdlDocument = _das_idl_parser.IdlDocument
TypeInfo = _das_idl_parser.TypeInfo
parse_idl_file = _das_idl_parser.parse_idl_file


# ---------------------------------------------------------------------------
# Base TypeMapper
# ---------------------------------------------------------------------------

class IpcBaseTypeMapper:
    """IPC 类型映射器基类

    将 IDL 类型映射到序列化方法：
    1. 基本类型 -> SerializerWriter/SerializerReader 方法
    2. Struct 类型 -> 内联展开每个字段的 Write*/Read* 调用
    """

    # IDL 基本类型 -> (C++类型, Write方法名, Read方法名)
    # 包含 proxy 原始的所有条目（stub 的条目是超集，通过子类扩展）
    TYPE_MAP = {
        # 有符号整数
        'int8': ('int8_t', 'WriteInt8', 'ReadInt8'),
        'int16': ('int16_t', 'WriteInt16', 'ReadInt16'),
        'int32': ('int32_t', 'WriteInt32', 'ReadInt32'),
        'int32_t': ('int32_t', 'WriteInt32', 'ReadInt32'),
        'int64': ('int64_t', 'WriteInt64', 'ReadInt64'),
        'int64_t': ('int64_t', 'WriteInt64', 'ReadInt64'),
        'int': ('int32_t', 'WriteInt32', 'ReadInt32'),
        # 无符号整数
        'uint8': ('uint8_t', 'WriteUInt8', 'ReadUInt8'),
        'uint16': ('uint16_t', 'WriteUInt16', 'ReadUInt16'),
        'uint32': ('uint32_t', 'WriteUInt32', 'ReadUInt32'),
        'uint32_t': ('uint32_t', 'WriteUInt32', 'ReadUInt32'),
        'uint64': ('uint64_t', 'WriteUInt64', 'ReadUInt64'),
        'uint64_t': ('uint64_t', 'WriteUInt64', 'ReadUInt64'),
        'uint': ('uint32_t', 'WriteUInt32', 'ReadUInt32'),
        # 浮点数
        'float': ('float', 'WriteFloat', 'ReadFloat'),
        'double': ('double', 'WriteDouble', 'ReadDouble'),
        # 布尔
        'bool': ('bool', 'WriteBool', 'ReadBool'),
        # 字符
        'char': ('char', 'WriteInt8', 'ReadInt8'),
        # 大小类型
        'size_t': ('size_t', 'WriteUInt64', 'ReadUInt64'),
    }

    # 特殊类型（需要特殊处理）— 基类版本，子类可扩展
    SPECIAL_TYPES = {
        'string': ('std::string', 'WriteString', 'ReadString'),
        'DasGuid': ('DasGuid', 'WriteGuid', 'ReadGuid'),
        'DasResult': ('DasResult', 'WriteInt32', 'ReadInt32'),
        'DasBool': ('DasBool', 'WriteBool', 'ReadBool'),
    }

    def __init__(self, document: IdlDocument):
        """初始化类型映射器

        Args:
            document: IDL 文档，用于获取 struct 定义
        """
        self.document = document
        # 收集所有 struct 类型
        self.struct_types = set()
        self.struct_defs = {}  # name -> StructDef，用于内联序列化展开
        for struct in document.structs:
            self.struct_types.add(struct.name)
            self.struct_defs[struct.name] = struct
            # 也支持带命名空间的 struct
            if struct.namespace:
                full_name = f"{struct.namespace}::{struct.name}"
                self.struct_types.add(full_name)
                self.struct_defs[full_name] = struct

        # 收集所有 enum 类型（来自当前文档）
        self.enum_types = set()
        for enum in document.enums:
            self.enum_types.add(enum.name)
            if enum.namespace:
                self.enum_types.add(f"{enum.namespace}::{enum.name}")

        # 收集接口名 -> 命名空间映射（用于跨命名空间类型引用）
        self.interface_namespaces = {}  # interface_name -> namespace
        self.interface_header_files = {}  # interface_name -> header_filename
        for iface in document.interfaces:
            self.interface_namespaces[iface.name] = iface.namespace

    def get_type_info(self, idl_type: str):
        """获取类型信息

        Args:
            idl_type: IDL 类型名称

        Returns:
            (cpp_type, write_method, read_method, is_struct) 或 None
        """
        # 检查基本类型
        if idl_type in self.TYPE_MAP:
            cpp_type, write_method, read_method = self.TYPE_MAP[idl_type]
            return (cpp_type, write_method, read_method, False)

        # 检查特殊类型
        if idl_type in self.SPECIAL_TYPES:
            cpp_type, write_method, read_method = self.SPECIAL_TYPES[idl_type]
            return (cpp_type, write_method, read_method, False)

        # 检查 struct 类型
        if idl_type in self.struct_types:
            return (idl_type, None, None, True)

        # 检查 enum 类型 -> 映射为 int32_t
        if idl_type in self.enum_types:
            return ('int32_t', 'WriteInt32', 'ReadInt32', False)

        return None

    def is_basic_type(self, idl_type: str) -> bool:
        """检查是否是基本类型（可以直接序列化）"""
        return idl_type in self.TYPE_MAP or idl_type in self.SPECIAL_TYPES

    def is_struct_type(self, idl_type: str) -> bool:
        """检查是否是 struct 类型"""
        return idl_type in self.struct_types

    def get_interface_name(self, idl_type: str) -> str:
        """从接口指针类型提取接口名"""
        type_name = idl_type.split("::")[-1]

        is_pointer = type_name.endswith("*")
        is_smart_ptr = type_name.endswith("Ptr")

        if is_pointer:
            return type_name[:-1]  # 去除 *
        elif is_smart_ptr:
            return type_name[:-3]  # 去除 Ptr
        return type_name

    def load_namespaces_from_abi_dir(self, abi_dir: str) -> None:
        """从 ABI 头文件目录扫描所有接口的命名空间

        扫描 abi_dir 下的所有 .h 文件，提取 namespace 和接口前向声明，
        构建 interface_namespaces 映射。

        Args:
            abi_dir: ABI 头文件目录路径
        """
        abi_path = Path(abi_dir)
        if not abi_path.is_dir():
            return

        # 正则表达式匹配 namespace 块中的 DAS_INTERFACE 前向声明
        ns_block_pattern = re.compile(
            r'namespace\s+([\w:]+)\s*\{',
            re.DOTALL
        )
        iface_decl_pattern = re.compile(r'DAS_INTERFACE\s+(\w+)\s*;')

        for header_file in abi_path.glob("*.h"):
            try:
                content = header_file.read_text(encoding='utf-8')
                # 查找所有 namespace 块及其中的 DAS_INTERFACE 声明
                pos = 0
                open_braces = 0
                current_ns = ""
                while pos < len(content):
                    # 查找 namespace 声明
                    ns_match = ns_block_pattern.search(content, pos)
                    if ns_match is None:
                        break
                    current_ns = ns_match.group(1)
                    brace_start = ns_match.end()

                    # 追踪大括号以找到 namespace 块的结束
                    open_braces = 1
                    scan_pos = brace_start
                    while open_braces > 0 and scan_pos < len(content):
                        if content[scan_pos] == '{':
                            open_braces += 1
                        elif content[scan_pos] == '}':
                            open_braces -= 1
                        scan_pos += 1

                    # 在 namespace 块中查找 DAS_INTERFACE 声明
                    ns_content = content[brace_start:scan_pos]
                    for iface_match in iface_decl_pattern.finditer(ns_content):
                        iface_name = iface_match.group(1)
                        if iface_name not in self.interface_namespaces:
                            self.interface_namespaces[iface_name] = current_ns
                        # 记录接口到头文件的映射
                        if iface_name not in self.interface_header_files:
                            self.interface_header_files[iface_name] = header_file.name

                    pos = scan_pos
            except Exception:
                pass

    def load_external_definitions(self, documents: list) -> None:
        """Load enum and struct definitions from pre-parsed IdlDocument objects.

        Accepts already-parsed documents (e.g. from resolve_import_chain)
        to avoid redundant parsing overhead.
        """
        for ext_doc in documents:
            for enum in ext_doc.enums:
                self.enum_types.add(enum.name)
                if enum.namespace:
                    self.enum_types.add(f"{enum.namespace}::{enum.name}")
            for struct in ext_doc.structs:
                if struct.name not in self.struct_defs:
                    self.struct_types.add(struct.name)
                    self.struct_defs[struct.name] = struct
                    if struct.namespace:
                        full_name = f"{struct.namespace}::{struct.name}"
                        self.struct_types.add(full_name)
                        self.struct_defs[full_name] = struct


# ---------------------------------------------------------------------------
# Proxy-specific subclass
# ---------------------------------------------------------------------------

class ProxyTypeMapper(IpcBaseTypeMapper):
    """Proxy TypeMapper — additionally loads interface namespaces from external IDL files."""

    def load_external_definitions(self, documents: list) -> None:
        """Load enum, struct, and interface definitions from pre-parsed IdlDocument objects.

        Accepts already-parsed documents (e.g. from resolve_import_chain)
        to avoid redundant parsing overhead.
        """
        for ext_doc in documents:
            for enum in ext_doc.enums:
                self.enum_types.add(enum.name)
                if enum.namespace:
                    self.enum_types.add(f"{enum.namespace}::{enum.name}")
            for struct in ext_doc.structs:
                if struct.name not in self.struct_defs:
                    self.struct_types.add(struct.name)
                    self.struct_defs[struct.name] = struct
                    if struct.namespace:
                        full_name = f"{struct.namespace}::{struct.name}"
                        self.struct_types.add(full_name)
                        self.struct_defs[full_name] = struct
            # Load interface namespaces from external IDL files
            # This is critical for cross-namespace interface references
            for iface in ext_doc.interfaces:
                if iface.name not in self.interface_namespaces:
                    self.interface_namespaces[iface.name] = iface.namespace


# ---------------------------------------------------------------------------
# Stub-specific subclass
# ---------------------------------------------------------------------------

class StubTypeMapper(IpcBaseTypeMapper):
    """Stub TypeMapper — adds extra TYPE_MAP entries, IDasReadOnlyString to
    SPECIAL_TYPES, and interface header file fallback mappings."""

    TYPE_MAP = {
        **IpcBaseTypeMapper.TYPE_MAP,
        # Stub additionally handles _t-suffixed and unsigned char types
        'int8_t': ('int8_t', 'WriteInt8', 'ReadInt8'),
        'int16_t': ('int16_t', 'WriteInt16', 'ReadInt16'),
        'int32_t': ('int32_t', 'WriteInt32', 'ReadInt32'),
        'int64_t': ('int64_t', 'WriteInt64', 'ReadInt64'),
        'uint8_t': ('uint8_t', 'WriteUInt8', 'ReadUInt8'),
        'uint16_t': ('uint16_t', 'WriteUInt16', 'ReadUInt16'),
        'uint32_t': ('uint32_t', 'WriteUInt32', 'ReadUInt32'),
        'uint64_t': ('uint64_t', 'WriteUInt64', 'ReadUInt64'),
        'unsigned char': ('uint8_t', 'WriteUInt8', 'ReadUInt8'),
    }

    SPECIAL_TYPES = {
        **IpcBaseTypeMapper.SPECIAL_TYPES,
        'IDasReadOnlyString': ('IDasReadOnlyString', 'WriteString', 'ReadString'),
    }

    def load_namespaces_from_abi_dir(self, abi_dir: str) -> None:
        """从 ABI 头文件目录扫描所有接口的命名空间

        与基类相同，但额外添加已知的接口头文件回退映射。
        """
        # Call base implementation
        super().load_namespaces_from_abi_dir(abi_dir)

        # 添加已知的接口头文件映射回退（多个接口可能在同一个头文件中）
        # 这些接口在同一个 IDL 文件中定义但共享一个 ABI 头文件
        fallback_mappings = {
            "IDasWeakReferenceSource": "IDasWeakReference.h",
        }
        for iface_name, header_name in fallback_mappings.items():
            if iface_name not in self.interface_header_files:
                self.interface_header_files[iface_name] = header_name
