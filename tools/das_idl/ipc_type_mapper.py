"""IPC TypeMapper shared base class and subclasses.

Consolidates the near-identical type mapping logic used by
das_ipc_proxy_generator (ProxyTypeMapper) and
das_ipc_stub_generator (StubTypeMapper).
"""

import os
from pathlib import Path
from typing import Dict, List, Optional

import importlib
import sys

# ---------------------------------------------------------------------------
# Parser type imports — support both package import and direct script execution
# ---------------------------------------------------------------------------

try:
    from . import das_idl_parser as _das_idl_parser
    from . import shared_utils as _shared_utils
except ImportError:
    this_dir = str(Path(__file__).resolve().parent)
    if this_dir not in sys.path:
        sys.path.insert(0, this_dir)
    _das_idl_parser = importlib.import_module("das_idl_parser")
    _shared_utils = importlib.import_module("shared_utils")

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
        # _t 后缀的 8/16 位变体 — IDL 中 DasDate 等结构体直接使用 stdint 别名，
        # proxy 与 stub 都需要识别，故放在基类而非子类
        'int8_t': ('int8_t', 'WriteInt8', 'ReadInt8'),
        'int16_t': ('int16_t', 'WriteInt16', 'ReadInt16'),
        # 无符号整数
        'uint8': ('uint8_t', 'WriteUInt8', 'ReadUInt8'),
        'uint16': ('uint16_t', 'WriteUInt16', 'ReadUInt16'),
        'uint32': ('uint32_t', 'WriteUInt32', 'ReadUInt32'),
        'uint32_t': ('uint32_t', 'WriteUInt32', 'ReadUInt32'),
        'uint64': ('uint64_t', 'WriteUInt64', 'ReadUInt64'),
        'uint64_t': ('uint64_t', 'WriteUInt64', 'ReadUInt64'),
        'uint': ('uint32_t', 'WriteUInt32', 'ReadUInt32'),
        # _t 后缀的 8/16 位变体 — 见上方 int8_t/int16_t 注释
        'uint8_t': ('uint8_t', 'WriteUInt8', 'ReadUInt8'),
        'uint16_t': ('uint16_t', 'WriteUInt16', 'ReadUInt16'),
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
        # 合并导入 IDL 文件中的接口命名空间（由 resolve_types() 填充）
        for name, ns in document.imported_interface_namespaces.items():
            if name not in self.interface_namespaces:
                self.interface_namespaces[name] = ns

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

    def load_external_definitions(self, imported_docs: dict) -> None:
        """Load enum, struct, and interface definitions from imported IDL documents.

        Args:
            imported_docs: Dict mapping absolute IDL file path -> parsed IdlDocument.
                           Used to derive interface_header_files from IDL filenames.
        """
        for idl_path, ext_doc in imported_docs.items():
            # Derive ABI header filename from IDL filename:
            #   "DasJson.idl" -> "DasJson.h"
            header_name = _shared_utils.idl_path_to_header_name(idl_path)
            for iface in ext_doc.interfaces:
                if iface.name not in self.interface_header_files:
                    self.interface_header_files[iface.name] = header_name
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

    def load_external_definitions(self, imported_docs: dict) -> None:
        """Load enum, struct, and interface definitions from imported IDL documents.

        Args:
            imported_docs: Dict mapping absolute IDL file path -> parsed IdlDocument.
        """
        super().load_external_definitions(imported_docs)
        for idl_path, ext_doc in imported_docs.items():
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
    """Stub TypeMapper — adds extra TYPE_MAP entries and IDasReadOnlyString to
    SPECIAL_TYPES."""

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
