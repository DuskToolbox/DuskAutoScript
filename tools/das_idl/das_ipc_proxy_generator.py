"""
DAS IPC Proxy 代码生成器

从 IDL 接口定义生成 IPC Proxy 代码，用于客户端调用远程对象。

功能:
1. 为每个接口生成 Proxy 类，继承 IPCProxyBase
2. 使用 FNV-1a hash 计算 InterfaceId（基于接口 UUID）
3. 生成 MethodTable 元数据
4. 为每个方法生成代理调用代码
5. 写入 interface.json 到 cache_dir 用于 Registry 生成

输出文件命名: proxy/<InterfaceName>Proxy.h

Proxy 代码规范 (B6):
- static constexpr uint32_t InterfaceId = <FNV-1a hash of UUID>;
- static constexpr MethodMetadata MethodTable[] = {...};
- 继承 IPCProxyBase
- 每个方法:
  1. 序列化参数
  2. 填充消息头
  3. 发送消息（调用 SendRequest）
  4. 等待响应（同步）或返回 future（异步）
"""

import json
import os
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Any
import importlib
import sys

from ipc_common import fnv1a_hash_guid

# 既支持作为包内模块导入（tools.das_idl.*），也支持直接脚本运行。
try:
    from . import das_idl_parser as _das_idl_parser
except ImportError:
    this_dir = str(Path(__file__).resolve().parent)
    if this_dir not in sys.path:
        sys.path.insert(0, this_dir)
    _das_idl_parser = importlib.import_module("das_idl_parser")

IdlDocument = _das_idl_parser.IdlDocument
InterfaceDef = _das_idl_parser.InterfaceDef
MethodDef = _das_idl_parser.MethodDef
ParameterDef = _das_idl_parser.ParameterDef
TypeInfo = _das_idl_parser.TypeInfo
TypeKind = _das_idl_parser.TypeKind
ParamDirection = _das_idl_parser.ParamDirection
StructDef = _das_idl_parser.StructDef
parse_idl_file = _das_idl_parser.parse_idl_file


class ProxyTypeMapper:
    """Proxy 类型映射器

    将 IDL 类型映射到序列化方法：
    1. 基本类型 → SerializerWriter/SerializerReader 方法
    2. Struct 类型 → 内联展开每个字段的 Write*/Read* 调用
    """
    
    # IDL 基本类型 → (C++类型, Write方法名, Read方法名)
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
    
    # 特殊类型（需要特殊处理）
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
            # struct 类型使用内联字段逐一序列化
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

    def is_interface_type(self, idl_type: str) -> bool:
        """检查是否是接口指针类型（如 IDasXxx*, IDasXxxPtr）

        接口类型特征：
        - 以 "I" 开头
        - 以 "*" 结尾（指针）
        - 或者以 "Ptr" 结尾（智能指针）
        - 通常以 "Das" 结尾（如 IDasLogReader）
        """
        # 去除命名空间前缀
        type_name = idl_type.split("::")[-1]

        # 检查是否是指针类型
        is_pointer = type_name.endswith("*")
        is_smart_ptr = type_name.endswith("Ptr")

        if not (is_pointer or is_smart_ptr):
            return False

        # 提取接口名（去除指针标记）
        interface_name = type_name[:-1]  # 去除 *
        if is_smart_ptr:
            interface_name = type_name[:-3]  # 去除 Ptr

        # 接口名必须以 I 开头且包含 Das
        return interface_name.startswith("I") and "Das" in interface_name

    @staticmethod
    def is_interface_type_from_kind(type_info: TypeInfo) -> bool:
        """根据 TypeInfo.type_kind 判断是否是接口类型（推荐使用）"""
        return type_info.type_kind == TypeKind.INTERFACE

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
        # 先匹配 namespace 块，再提取其中的 DAS_INTERFACE 声明
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

    def load_external_definitions(self, idl_dirs: List[str]) -> None:
        """Load enum, struct, and interface definitions from all IDL files in given directories.

        This enables cross-IDL type references (e.g., DasRect from DasBasicTypes.idl
        used in methods defined in other IDL files, or IDasImage from ExportInterface
        used in methods defined in PluginInterface).
        """
        import glob
        for idl_dir in idl_dirs:
            for idl_path in glob.glob(os.path.join(idl_dir, "*.idl")):
                try:
                    ext_doc = parse_idl_file(idl_path)
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
                except Exception:
                    pass  # Skip unparseable files


def fnv1a_hash(data: str) -> int:
    """计算字符串的 FNV-1a 32-bit hash
    
    Args:
        data: 要计算 hash 的字符串
        
    Returns:
        32-bit FNV-1a hash 值
    """
    FNV_PRIME = 0x01000193
    FNV_OFFSET_BASIS = 0x811c9dc5
    
    hash_value = FNV_OFFSET_BASIS
    for char in data.encode('utf-8'):
        hash_value ^= char
        hash_value = (hash_value * FNV_PRIME) & 0xFFFFFFFF
    
    return hash_value




class IpcProxyGenerator:
    """IPC Proxy 代码生成器"""

    def __init__(self, document: IdlDocument, idl_file_path: Optional[str] = None,
                 extra_interface_namespaces: Optional[Dict[str, str]] = None):
        self.document = document
        self.idl_file_path = idl_file_path
        self.idl_file_name = os.path.basename(idl_file_path) if idl_file_path else None
        self.indent = "    "  # 4 空格缩进
        self.type_mapper = ProxyTypeMapper(document)
        # Load external enum/struct definitions from all IDL directories
        if idl_file_path:
            idl_base_dir = str(Path(idl_file_path).resolve().parent)
            self.type_mapper.load_external_definitions([idl_base_dir])
        # 额外的接口命名空间映射（来自其他 IDL 文件的接口）
        if extra_interface_namespaces:
            self.type_mapper.interface_namespaces.update(extra_interface_namespaces)

    @staticmethod
    def _properties_to_methods(properties: list) -> list:
        """将 PropertyDef 列表转换为 MethodDef 列表（getter/setter）"""
        methods = []
        for prop in properties:
            is_interface = (prop.type_info.type_kind == TypeKind.INTERFACE
                            and prop.type_info.pointer_level == 0)
            is_string = prop.type_info.base_type in ('string', 'IDasReadOnlyString')

            if prop.has_getter:
                if is_interface or is_string:
                    out_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=2, is_pointer=True,
                                        type_kind=prop.type_info.type_kind)
                    params = [ParameterDef(name="pp_out", type_info=out_type, direction=ParamDirection.OUT)]
                else:
                    out_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=1, is_pointer=True,
                                        type_kind=prop.type_info.type_kind)
                    params = [ParameterDef(name="p_out", type_info=out_type, direction=ParamDirection.OUT)]
                methods.append(MethodDef(name=f"Get{prop.name}", return_type=TypeInfo(base_type="DasResult", pointer_level=0), parameters=params))

            if prop.has_setter:
                if is_interface:
                    in_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=1, is_pointer=True,
                                       type_kind=prop.type_info.type_kind)
                elif is_string:
                    in_type = TypeInfo(base_type="IDasReadOnlyString", pointer_level=1, is_pointer=True,
                                       type_kind=prop.type_info.type_kind)
                else:
                    in_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=0,
                                       type_kind=prop.type_info.type_kind)
                params = [ParameterDef(name="p_value", type_info=in_type, direction=ParamDirection.IN)]
                methods.append(MethodDef(name=f"Set{prop.name}", return_type=TypeInfo(base_type="DasResult", pointer_level=0), parameters=params))

        return methods

    def _collect_all_methods(self, interface: InterfaceDef) -> list:
        """收集接口及其所有父接口的方法（深度优先，从根到叶）

        返回 [(method, defining_interface_name), ...] 列表，
        其中 defining_interface_name 是定义该方法的接口名。
        """
        # 构建接口查找表
        iface_map = {iface.name: iface for iface in self.document.interfaces}

        # 补充查找：从 IDL 目录中的其他文件解析缺失的父接口
        if self.idl_file_path:
            idl_dir = os.path.dirname(self.idl_file_path)
            if os.path.isdir(idl_dir):
                for f in os.listdir(idl_dir):
                    if f.endswith('.idl'):
                        fpath = os.path.join(idl_dir, f)
                        try:
                            doc = parse_idl_file(fpath)
                            for iface in doc.interfaces:
                                if iface.name not in iface_map:
                                    iface_map[iface.name] = iface
                        except Exception:
                            pass

        # 收集继承链（从根到叶）
        chain = []
        current = interface
        visited = set()
        while current and current.name != "IDasBase" and current.name not in visited:
            chain.append(current)
            visited.add(current.name)
            current = iface_map.get(current.base_interface)
        chain.reverse()  # 从根到叶

        # 收集所有方法，每个方法属于定义它的接口
        all_methods = []
        for iface in chain:
            for method in iface.methods:
                all_methods.append((method, iface))
            # 属性生成的 getter/setter 也作为方法
            for method in self._properties_to_methods(iface.properties):
                all_methods.append((method, iface))

        return all_methods
    
    def _file_header(self, guard_name: str, interface_name: str, interface: InterfaceDef = None) -> str:
        """生成文件头"""
        timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

        idl_file_comment = ""
        abi_header_name = ""
        if self.idl_file_name:
            idl_file_comment = f"// Source IDL file: {self.idl_file_name}\n"
            abi_header_name = os.path.splitext(self.idl_file_name)[0] + ".h"

        result = f"""#if !defined({guard_name})
#define {guard_name}

// This file is automatically generated by DAS IPC Proxy Generator
// Generated at: {timestamp}
{idl_file_comment}// !!! DO NOT EDIT !!!
//
// IPC Proxy for {interface_name}
//

#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/InterfaceParamSerialization.h>
#include <das/Core/IPC/MemorySerializer.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/Serializer.h>
#include <das/Core/Logger/Logger.h>
#include <atomic>
#include <cstdint>
#include <string>

#include "{abi_header_name}"

"""

        # 检查是否需要 <cstring> (std::strlen for IDasReadOnlyString [in] params)
        needs_cstring = False
        if interface:
            for method in interface.methods:
                for param in method.parameters:
                    if param.direction == ParamDirection.IN and param.type_info.base_type == "IDasReadOnlyString":
                        needs_cstring = True
                        break
                if needs_cstring:
                    break
            # Also check property-generated methods
            for prop in getattr(interface, 'properties', []):
                for param in getattr(prop, 'parameters', []):
                    if getattr(param, 'direction', None) == ParamDirection.IN and param.type_info.base_type == "IDasReadOnlyString":
                        needs_cstring = True
                        break
                if needs_cstring:
                    break

        # 检查是否需要 <vector> (for [binary_buffer] methods)
        has_binary_buffer = False
        if interface:
            all_method_defs = self._collect_all_methods(interface)
            has_binary_buffer = any(
                m.attributes.get('binary_buffer', False)
                for m, _ in all_method_defs
            )

        # 添加方法参数中使用的接口类型的头文件
        if interface:
            interface_includes = self._collect_interface_includes(interface)
            if interface_includes:
                includes_str = "\n".join(
                    f'#include "{inc}"' for inc in interface_includes
                )
                result += f"{includes_str}\n"

        # 条件添加 <cstring> (for std::strlen with IDasReadOnlyString [in] params)
        if needs_cstring:
            result += "#include <cstring>\n"

        # 条件添加 <vector> (for [binary_buffer] methods)
        if has_binary_buffer:
            result += "#include <vector>\n"
            result += "#include <das/Core/IPC/AsyncIpcTransport.h>\n"
            result += "#include <das/Core/IPC/ConnectionManager.h>\n"
            result += "#include <das/Core/IPC/IpcCommandHandler.h>\n"
            result += "#include <das/Core/IPC/SharedMemoryPool.h>\n"

        return result
    
    def _file_footer(self, guard_name: str) -> str:
        """生成文件尾"""
        return f"""
#endif // {guard_name}
"""
    
    def _generate_namespace_open(self, namespace: str) -> str:
        """生成命名空间开始标记（Allman 风格）"""
        if namespace:
            parts = namespace.split("::")
            result = []
            for i, part in enumerate(parts):
                indent = "    " * i
                result.append(f"{indent}namespace {part}")
                result.append(f"{indent}{{")
            return "\n".join(result) + "\n"
        return ""
    
    def _generate_namespace_close(self, namespace: str) -> str:
        """生成命名空间结束标记（Allman 风格）"""
        if namespace:
            parts = namespace.split("::")
            result = []
            for i in range(len(parts) - 1, -1, -1):
                indent = "    " * i
                result.append(f"{indent}}}")
            result.append(f"// namespace {namespace}")
            return "\n".join(result) + "\n"
        return ""
    
    def _get_cpp_type(self, type_info: TypeInfo, current_namespace: str = "") -> str:
        """将 IDL 类型转换为 C++ 类型

        Args:
            type_info: 类型信息
            current_namespace: 当前接口的命名空间（用于跨命名空间类型限定）
        """
        TYPE_MAP = {
            'bool': 'bool',
            'int8': 'int8_t',
            'int16': 'int16_t',
            'int32': 'int32_t',
            'int32_t': 'int32_t',
            'int64': 'int64_t',
            'int64_t': 'int64_t',
            'uint8': 'uint8_t',
            'uint16': 'uint16_t',
            'uint32': 'uint32_t',
            'uint32_t': 'uint32_t',
            'uint64': 'uint64_t',
            'uint64_t': 'uint64_t',
            'float': 'float',
            'double': 'double',
            'size_t': 'size_t',
            'int': 'int32_t',
            'uint': 'uint32_t',
            'DasResult': 'DasResult',
            'DasBool': 'DasBool',
            'DasGuid': 'DasGuid',
            'char': 'char',
            'string': 'std::string',
        }

        base = TYPE_MAP.get(type_info.base_type, type_info.base_type)

        # 检查是否是跨命名空间的接口类型，需要添加完全限定名
        if base not in TYPE_MAP and current_namespace:
            # 检查是否是接口名（在 interface_namespaces 映射中）
            iface_ns = self.type_mapper.interface_namespaces.get(base)
            if iface_ns and iface_ns != current_namespace:
                base = f"{iface_ns}::{base}"

        result = ""
        if type_info.is_const:
            result += "const "

        result += base

        if type_info.is_pointer:
            result += "*" * type_info.pointer_level

        if type_info.is_reference:
            result += "&"

        return result
    
    def _get_interface_short_name(self, interface_name: str) -> str:
        """从接口名获取短名称（去掉 I 前缀）
        
        IDasLogger -> DasLogger
        """
        if interface_name.startswith('IDas'):
            return interface_name[1:]
        if interface_name.startswith('I') and len(interface_name) > 1:
            return interface_name[1:]
        return interface_name

    def _collect_interface_includes(self, interface: InterfaceDef) -> List[str]:
        """收集接口方法中使用的接口类型的头文件路径

        遍历所有方法，检查返回类型和参数类型是否为接口类型，
        如果是则收集对应的头文件路径（约定为 {InterfaceName}.h）。

        注意：base_type 是纯类型名（不含 *），所以不能直接使用
        is_interface_type（它期望输入以 * 或 Ptr 结尾）。
        这里通过 type_info.is_pointer + base_type 命名模式来判断。

        核心类型（如 IDasReadOnlyString、IDasStopToken）不需要额外 include，
        因为它们已经通过 ABI 头文件中的 include 链可见。

        同一 IDL 文件中定义的接口类型不需要额外 include，
        因为它们已通过主 ABI 头文件（{idl_file_name}.h）的前向声明可见。
        """
        # 核心类型
        CORE_TYPES = {
            "IDasReadOnlyString",
            "IDasReadOnlyGuidVector",
            "IDasStopToken",
            "IDasBinaryBuffer",
            "IDasString",
            "IDasBase",
            "IDasTypeInfo",
        }

        # 收集当前 IDL 文档中定义的所有接口名
        local_iface_names = set()
        for iface in self.document.interfaces:
            local_iface_names.add(iface.name)

        includes = set()

        def _is_iface_ptr(type_info: TypeInfo) -> bool:
            if not type_info.is_pointer:
                return False
            name = type_info.base_type.split("::")[-1]
            return name.startswith("I") and "Das" in name

        def _iface_name_from_type(type_info: TypeInfo) -> str:
            return type_info.base_type.split("::")[-1]

        for method in interface.methods:
            if _is_iface_ptr(method.return_type):
                iface_name = _iface_name_from_type(method.return_type)
                if iface_name not in CORE_TYPES and iface_name not in local_iface_names:
                    header = self.type_mapper.interface_header_files.get(iface_name, f"{iface_name}.h")
                    includes.add(header)

            for param in method.parameters:
                if _is_iface_ptr(param.type_info):
                    iface_name = _iface_name_from_type(param.type_info)
                    if iface_name not in CORE_TYPES and iface_name not in local_iface_names:
                        header = self.type_mapper.interface_header_files.get(iface_name, f"{iface_name}.h")
                        includes.add(header)

        # 排除主接口自身的头文件（已通过 abi_header 包含）
        short_name = self._get_interface_short_name(interface.name)
        includes.discard(f"{interface.name}.h")
        includes.discard(f"{short_name}.h")

        return sorted(includes)

    def _generate_method_signature(self, interface: InterfaceDef, method: MethodDef, current_namespace: str = "") -> str:
        """生成方法签名

        Args:
            interface: 当前接口定义
            method: 方法定义
            current_namespace: 当前接口的命名空间（用于跨命名空间类型限定）
        """
        return_type = self._get_cpp_type(method.return_type, current_namespace)

        params = []
        for param in method.parameters:
            if param.direction == ParamDirection.OUT and param.type_info.base_type in self.type_mapper.interface_namespaces:
                # Match ABI's get_out_param_type: interface [out] params are always ITypeName**
                cpp_type = self._get_cpp_type(param.type_info, current_namespace)
                base_type = cpp_type.rstrip('*').rstrip()
                param_type = f"{base_type}**"
            else:
                param_type = self._get_cpp_type(param.type_info, current_namespace)
            params.append(f"{param_type} {param.name}")

        params_str = ", ".join(params) if params else ""
        return f"{return_type} {method.name}({params_str})"
    
    def _generate_method_body(self, interface: InterfaceDef, method: MethodDef, method_index: int, namespace_depth: int = 0) -> str:
        """生成方法体
        
        混合模式 (B7):
        - IsLocal=true: 直接调用本地实现（无序列化）
        - IsLocal=false: IPC 序列化/反序列化
        """
        indent = "    " * (namespace_depth + 2)  # class + public
        inner_indent = "    " * (namespace_depth + 3)
        lines = []
        
        interface_short_name = self._get_interface_short_name(interface.name)
        request_type = f"{interface_short_name}_{method.name}_Request"
        response_type = f"{interface_short_name}_{method.name}_Response"
        
        return_type = method.return_type.base_type
        has_return = return_type != 'void'
        
        # 构建参数列表
        param_names = [param.name for param in method.parameters]
        params_call = ", ".join(param_names) if param_names else ""
        
        # 使用 if constexpr (IsLocal) 编译时分支
        lines.append(f"{indent}if constexpr (IsLocal)")
        lines.append(f"{indent}{{")
        lines.append(f"{inner_indent}// Local mode: direct call (no serialization)")
        
        # 本地模式：直接调用
        if has_return:
            cpp_return_type = self._get_cpp_type(method.return_type)
            lines.append(f"{inner_indent}return local_impl_->{method.name}({params_call});")
        else:
            lines.append(f"{inner_indent}local_impl_->{method.name}({params_call});")
            lines.append(f"{inner_indent}return DAS_S_OK;")
        
        lines.append(f"{indent}}}")
        lines.append(f"{indent}else")
        lines.append(f"{indent}{{")
        lines.append(f"{inner_indent}// IPC mode: serialize + send + receive")
        
        # IPC 模式：原有逻辑
        # Step 1: 创建请求消息
        lines.append(f"{inner_indent}// Create request message")
        lines.append(f"{inner_indent}{request_type} request;")
        lines.append(f"{inner_indent}request.header.object_id = EncodeObjectId(target_);")
        lines.append(f"{inner_indent}request.header.interface_id = InterfaceId;")
        lines.append(f"{inner_indent}request.header.method_id = {method_index};")
        lines.append(f"{inner_indent}request.header.call_id = next_call_id_++;")
        lines.append(f"{inner_indent}request.header.flags = 0;")
        lines.append("")
        
        # Step 2: 序列化参数（占位符）
        lines.append(f"{inner_indent}// TODO: Serialize parameters")
        for param in method.parameters:
            param_type = self._get_cpp_type(param.type_info)
            lines.append(f"{inner_indent}// {param.name}: {param_type}")
        lines.append("")
        
        # Step 3: 发送消息
        lines.append(f"{inner_indent}// Send request and wait for response")
        lines.append(f"{inner_indent}{response_type} response;")
        lines.append(f"{inner_indent}DasResult ipc_result = transport_.SendAndWait(")
        lines.append(f"{inner_indent}    &request, sizeof(request),")
        lines.append(f"{inner_indent}    &response, sizeof(response));")
        lines.append("")
        
        # Step 4: 处理响应
        lines.append(f"{inner_indent}// Check result")
        lines.append(f"{inner_indent}if (DAS::IsFailed(ipc_result))")
        lines.append(f"{inner_indent}{{")
        lines.append(f"{inner_indent}    return ipc_result;")
        lines.append(f"{inner_indent}}}")
        lines.append("")
        
        # Step 5: 返回值处理
        if has_return:
            lines.append(f"{inner_indent}// TODO: Deserialize return value")
            lines.append(f"{inner_indent}// Return placeholder")
            lines.append(f"{inner_indent}return DAS_E_NOT_IMPLEMENTED;")
        else:
            lines.append(f"{inner_indent}return DAS_S_OK;")
        
        lines.append(f"{indent}}}")
        
        return "\n".join(lines)
    
    def _generate_proxy_class(self, interface: InterfaceDef, namespace_depth: int = 0) -> str:
        """为接口生成 Proxy 类，继承 DasProxyBase 和接口"""
        lines = []
        indent = "    " * namespace_depth
        class_indent = "    " * (namespace_depth + 1)
        method_indent = "    " * (namespace_depth + 2)
        inner_indent = "    " * (namespace_depth + 3)
        
        interface_short_name = self._get_interface_short_name(interface.name)
        class_name = f"{interface_short_name}Proxy"
        interface_id = fnv1a_hash_guid(interface.uuid)
        
        lines.append(f"{indent}class {class_name} : public DasProxyBase<{interface.name}>, public {interface.name}")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}public:")
        lines.append(f"{class_indent}static constexpr uint32_t InterfaceId = 0x{interface_id:08X}u;")
        lines.append("")

        # 收集继承链全部方法
        all_methods = self._collect_all_methods(interface)

        if all_methods:
            lines.append(f"{class_indent}static constexpr MethodMetadata MethodTable[] = {{")
            for i, (method, defining_iface) in enumerate(all_methods):
                method_hash = fnv1a_hash(f"{interface.name}::{method.name}")
                lines.append(f"{method_indent}{{ {i}, \"{method.name}\", 0x{method_hash:08X}u }},")
            lines.append(f"{class_indent}}};")
        else:
            lines.append(f"{class_indent}static constexpr MethodMetadata MethodTable[] = {{{{}}}};")
        lines.append("")
        lines.append(f"{class_indent}{class_name}(")
        lines.append(f"{class_indent}    const ObjectId& object_id,")
        lines.append(f"{class_indent}    IpcRunLoop& run_loop,")
        lines.append(f"{class_indent}    std::weak_ptr<BusinessThread> business_thread,")
        lines.append(f"{class_indent}    DistributedObjectManager& object_manager)")
        lines.append(f"{class_indent}    : DasProxyBase<{interface.name}>(InterfaceId, object_id, run_loop, std::move(business_thread), object_manager)")
        lines.append(f"{class_indent}{{")
        lines.append(f"{class_indent}}}")
        lines.append("")

        # Check if interface has [binary_buffer] methods — need data_cache_ member
        all_method_defs = self._collect_all_methods(interface)
        has_binary_buffer = any(
            m.attributes.get('binary_buffer', False)
            for m, _ in all_method_defs
        )

        if has_binary_buffer:
            lines.append(f"{indent}private:")
            lines.append(f"{class_indent}std::vector<uint8_t> data_cache_;")
            lines.append("")
            lines.append(f"{indent}public:")

        # AddRef/Release final 实现
        lines.append(f"{class_indent}uint32_t AddRef() final")
        lines.append(f"{class_indent}{{")
        lines.append(f"{method_indent}return ++ref_count_;")
        lines.append(f"{class_indent}}}")
        lines.append("")
        lines.append(f"{class_indent}uint32_t Release() final")
        lines.append(f"{class_indent}{{")
        lines.append(f"{method_indent}uint32_t count = --ref_count_;")
        lines.append(f"{method_indent}if (count == 0)")
        lines.append(f"{method_indent}{{")
        lines.append(f"{inner_indent}DAS_CORE_LOG_TRACE(\"Proxy {class_name} ref_count reached 0, cleaning up\");")
        lines.append(f"{inner_indent}ProxyFactory::GetInstance().RemoveFromCache(GetObjectId());")
        lines.append(f"{inner_indent}delete this;")
        lines.append(f"{method_indent}}}")
        lines.append(f"{method_indent}return count;")
        lines.append(f"{class_indent}}}")
        lines.append("")
        # QueryInterface override — 委托给 DasProxyBase::QueryInterfaceRemote
        lines.append(f"{class_indent}DasResult QueryInterface(const DasGuid& iid, void** pp_object) override")
        lines.append(f"{class_indent}{{")
        lines.append(f"{method_indent}return QueryInterfaceRemote(iid, pp_object);")
        lines.append(f"{class_indent}}}")
        lines.append("")
        
        for i, (method, defining_iface) in enumerate(all_methods):
            method_sig = self._generate_method_signature(interface, method, interface.namespace)
            lines.append(f"{class_indent}{method_sig} override")
            lines.append(f"{class_indent}{{")
            method_body = self._generate_method_body_v2(interface, method, i, namespace_depth)
            for line in method_body.splitlines():
                lines.append(f"{line}")
            lines.append(f"{class_indent}}}")
            lines.append("")
        
        lines.append(f"{indent}private:")
        lines.append(f"{class_indent}std::atomic<uint32_t> ref_count_{{1}};")
        lines.append(f"{indent}}};")
        lines.append("")
        
        return "\n".join(lines)
    
    # 固定大小类型映射表（用于零堆分配栈上结构体）
    FIXED_SIZES = {
        'int8': 1, 'uint8': 1,
        'int16': 2, 'uint16': 2,
        'int32': 4, 'uint32': 4, 'int32_t': 4, 'uint32_t': 4,
        'int64': 8, 'uint64': 8, 'int64_t': 8, 'uint64_t': 8,
        'int': 4, 'uint': 4,
        'float': 4, 'double': 8,
        'bool': 1, 'char': 1,
        'size_t': 8,
        'DasGuid': 16,
        'DasResult': 4,
        'DasBool': 1,
    }

    def _is_fixed_size_method(self, method: MethodDef) -> bool:
        """检查方法所有 [in] 参数是否都是固定大小（无字符串、无变长数据）"""
        FIXED_SIZE_SPECIAL = {'DasGuid', 'DasResult', 'DasBool'}
        for param in method.parameters:
            if param.direction == ParamDirection.OUT:
                continue
            bt = param.type_info.base_type
            # 字符串类型是变长的
            if bt == 'IDasReadOnlyString' or bt == 'string':
                return False
            # 基本类型映射中的类型是固定的
            if bt in self.type_mapper.TYPE_MAP:
                continue
            # 固定特殊类型
            if bt in FIXED_SIZE_SPECIAL:
                continue
            # 枚举类型是固定的（int32_t）
            if bt in self.type_mapper.enum_types:
                continue
            # Struct 类型：检查所有字段是否固定（递归）
            if bt in self.type_mapper.struct_types:
                continue  # 基本字段的 struct 是固定的
            # 接口指针：ObjectId 编码为 uint64（8字节）
            if param.type_info.type_kind == TypeKind.INTERFACE and param.type_info.is_pointer:
                continue
            # 未知类型 — 不是固定大小
            return False
        return True

    def _get_param_fixed_size(self, param: ParameterDef) -> int:
        """获取参数占用的字节大小（固定大小参数）"""
        bt = param.type_info.base_type
        if bt in self.FIXED_SIZES:
            return self.FIXED_SIZES[bt]
        # 枚举类型：int32_t
        if bt in self.type_mapper.enum_types:
            return 4
        # Struct 类型：累加所有字段大小
        if bt in self.type_mapper.struct_defs:
            struct_def = self.type_mapper.struct_defs[bt]
            total = 0
            for field in struct_def.fields:
                field_bt = field.type_name
                total += self.FIXED_SIZES.get(field_bt, 0)
            return total
        # 接口指针：ObjectId 编码为 uint64
        if param.type_info.type_kind == TypeKind.INTERFACE and param.type_info.is_pointer:
            return 8
        return 0

    def _generate_struct_fields(self, param: ParameterDef, indent: str) -> List[str]:
        """生成 packed struct 字段声明（用于栈上零堆分配）"""
        lines = []
        bt = param.type_info.base_type

        if bt in self.FIXED_SIZES:
            cpp_type = self.type_mapper.TYPE_MAP.get(bt, (bt, '', ''))[0] if bt in self.type_mapper.TYPE_MAP else bt
            lines.append(f"{indent}{cpp_type} {param.name};")
        elif bt in self.type_mapper.enum_types:
            lines.append(f"{indent}int32_t {param.name};")
        elif bt in self.type_mapper.struct_defs:
            struct_def = self.type_mapper.struct_defs[bt]
            for field in struct_def.fields:
                field_cpp_type = self.type_mapper.TYPE_MAP.get(field.type_name, (field.type_name, '', ''))[0] if field.type_name in self.type_mapper.TYPE_MAP else field.type_name
                lines.append(f"{indent}{field_cpp_type} {param.name}_{field.name};")
        else:
            # 接口指针：编码为 uint64
            lines.append(f"{indent}uint64_t {param.name}_encoded_id;")
        return lines

    def _has_in_interface_params(self, method: MethodDef) -> bool:
        """检查方法的 [in] 参数中是否包含接口指针类型"""
        for param in method.parameters:
            if param.direction == ParamDirection.OUT:
                continue
            if self._is_param_interface(param):
                return True
        return False

    def _is_param_interface(self, param: ParameterDef) -> bool:
        """检查参数是否是接口指针类型（排除 IDasReadOnlyString 等特殊类型）"""
        # IDasReadOnlyString starts with 'I' and contains 'Das' but is NOT
        # an interface pointer — it's handled separately in _generate_serialize_param
        if param.type_info.base_type == "IDasReadOnlyString":
            return False
        # 使用 type_kind 进行类型判断
        return (param.type_info.type_kind == TypeKind.INTERFACE
                and param.type_info.is_pointer)

    def _generate_struct_fill(self, param: ParameterDef, prefix: str, indent: str) -> List[str]:
        """生成 packed struct 字段赋值代码"""
        lines = []
        bt = param.type_info.base_type

        if bt in self.FIXED_SIZES:
            # 基本类型：直接赋值（指针需要解引用）
            if param.type_info.is_pointer and param.type_info.pointer_level >= 1:
                lines.append(f"{indent}{prefix}{param.name} = *{param.name};")
            else:
                lines.append(f"{indent}{prefix}{param.name} = {param.name};")
        elif bt in self.type_mapper.enum_types:
            # 枚举类型：static_cast<int32_t>
            lines.append(f"{indent}{prefix}{param.name} = static_cast<int32_t>({param.name});")
        elif bt in self.type_mapper.struct_defs:
            # Struct 类型：逐一字段赋值
            struct_def = self.type_mapper.struct_defs[bt]
            for field in struct_def.fields:
                if param.type_info.is_pointer and param.type_info.pointer_level >= 1:
                    lines.append(f"{indent}{prefix}{param.name}_{field.name} = {param.name}->{field.name};")
                else:
                    lines.append(f"{indent}{prefix}{param.name}_{field.name} = {param.name}.{field.name};")
        else:
            # 接口指针：SerializeInInterfaceParam (3-arg) + guard Track + EncodeObjectId
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    Das::Core::IPC::ObjectId {param.name}_id{{}};")
            lines.append(f"{indent}    bool {param.name}_newly_registered = false;")
            lines.append(f"{indent}    ipc_result = Das::Core::IPC::SerializeInInterfaceParam({param.name}, GetObjectManager(), {param.name}_id, &{param.name}_newly_registered);")
            lines.append(f"{indent}    if (DAS::IsFailed(ipc_result))")
            lines.append(f"{indent}    {{")
            lines.append(f"{indent}        return ipc_result;")
            lines.append(f"{indent}    }}")
            lines.append(f"{indent}    pending_in_param_exports.Track({param.name}_id, {param.name}_newly_registered);")
            lines.append(f"{indent}    {prefix}{param.name}_encoded_id = Das::Core::IPC::EncodeObjectId({param.name}_id);")
            lines.append(f"{indent}}}")
        return lines

    def _generate_fixed_size_request_body(self, interface: InterfaceDef, method: MethodDef, method_index: int, in_params: List[ParameterDef], indent: str) -> List[str]:
        """生成零堆分配的栈上 packed struct 请求体"""
        lines = []
        struct_name = f"{interface.name}_{method.name}_RequestBody"

        # 生成 struct 定义
        lines.append(f"{indent}#pragma pack(push, 1)")
        lines.append(f"{indent}struct {struct_name} {{")
        lines.append(f"{indent}    // V3 Body Header (16 bytes)")
        lines.append(f"{indent}    uint32_t interface_id;")
        lines.append(f"{indent}    uint16_t method_id;")
        lines.append(f"{indent}    uint16_t reserved;")
        lines.append(f"{indent}    uint16_t session_id;")
        lines.append(f"{indent}    uint16_t generation;")
        lines.append(f"{indent}    uint32_t local_id;")

        # 生成参数字段
        for param in in_params:
            field_lines = self._generate_struct_fields(param, indent + "    ")
            lines.extend(field_lines)

        lines.append(f"{indent}}};")
        lines.append(f"{indent}#pragma pack(pop)")
        lines.append("")
        lines.append(f"{indent}DasResult ipc_result = DAS_S_OK;")
        lines.append("")

        # 填充 struct
        lines.append(f"{indent}{struct_name} req{{}};")
        lines.append(f"{indent}req.interface_id = InterfaceId;")
        lines.append(f"{indent}req.method_id = {method_index};")
        lines.append(f"{indent}req.reserved = 0;")
        lines.append(f"{indent}req.session_id = GetObjectId().session_id;")
        lines.append(f"{indent}req.generation = GetObjectId().generation;")
        lines.append(f"{indent}req.local_id = GetObjectId().local_id;")

        # 填充参数字段
        for param in in_params:
            fill_lines = self._generate_struct_fill(param, "req.", indent)
            lines.extend(fill_lines)

        return lines

    def _generate_variable_size_request_body(self, interface: InterfaceDef, method: MethodDef, method_index: int, in_params: List[ParameterDef], indent: str) -> List[str]:
        """生成使用 writer.Reserve(total_size) 的变长请求体（单次分配）"""
        lines = []
        lines.append(f"{indent}DasResult ipc_result = DAS_S_OK;")
        lines.append(f"{indent}(void)ipc_result;")
        lines.append("")

        # Phase 1: 预计算总大小
        lines.append(f"{indent}// Pre-calculate total body size for single allocation")
        lines.append(f"{indent}size_t total_body_size = 16;  // V3 Body Header")

        # 收集字符串参数用于长度计算
        string_params = []
        for param in in_params:
            if param.type_info.base_type == "IDasReadOnlyString":
                pn = param.name
                lines.append(f"{indent}const char* {pn}_u8 = nullptr;")
                lines.append(f"{indent}ipc_result = {pn}->GetUtf8(&{pn}_u8);")
                lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
                lines.append(f"{indent}{{")
                lines.append(f"{indent}    return ipc_result;")
                lines.append(f"{indent}}}")
                lines.append(f"{indent}const size_t {pn}_len = std::strlen({pn}_u8);")
                lines.append(f"{indent}total_body_size += 8 + {pn}_len;  // uint64 length prefix + string data")
                string_params.append(pn)
            else:
                size = self._get_param_fixed_size(param)
                lines.append(f"{indent}total_body_size += {size};  // {param.name}")

        lines.append("")

        # Phase 2: 创建 writer 并预分配
        lines.append(f"{indent}MemorySerializerWriter writer;")
        lines.append(f"{indent}writer.Reserve(total_body_size);")
        lines.append("")

        # Phase 3: 写入 V3 Header
        lines.append(f"{indent}// V3 Body Header")
        lines.append(f"{indent}ipc_result = writer.WriteUInt32(InterfaceId);")
        lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return ipc_result;")
        lines.append(f"{indent}}}")
        lines.append(f"{indent}ipc_result = writer.WriteUInt16({method_index});")
        lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return ipc_result;")
        lines.append(f"{indent}}}")
        lines.append(f"{indent}ipc_result = writer.WriteUInt16(0);")
        lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return ipc_result;")
        lines.append(f"{indent}}}")
        lines.append(f"{indent}ipc_result = writer.WriteUInt16(GetObjectId().session_id);")
        lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return ipc_result;")
        lines.append(f"{indent}}}")
        lines.append(f"{indent}ipc_result = writer.WriteUInt16(GetObjectId().generation);")
        lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return ipc_result;")
        lines.append(f"{indent}}}")
        lines.append(f"{indent}ipc_result = writer.WriteUInt32(GetObjectId().local_id);")
        lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return ipc_result;")
        lines.append(f"{indent}}}")

        # Phase 4: 写入参数（使用预取的字符串指针）
        lines.append(f"{indent}// V3 Body: parameters")
        for param in in_params:
            if param.type_info.base_type == "IDasReadOnlyString":
                pn = param.name
                lines.append(f"{indent}ipc_result = writer.WriteString({pn}_u8, {pn}_len);")
                lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
                lines.append(f"{indent}{{")
                lines.append(f"{indent}    return ipc_result;")
                lines.append(f"{indent}}}")
            else:
                # 使用现有的序列化方法
                serialize_code = self._generate_serialize_param(param, indent)
                lines.extend(serialize_code)

        return lines

    def _generate_method_body_v2(self, interface: InterfaceDef, method: MethodDef, method_index: int, namespace_depth: int = 0) -> str:
        """生成方法体，使用基类的 SendRequest 方法

        Generated Code Pattern:
        1. 检查目标是否是本地对象
        2. 如果是本地对象，直接调用本地实现
        3. 如果是远程对象，序列化输入参数
        4. 调用 SendRequest
        5. 反序列化远程返回码
        6. 如果远程返回码失败则返回
        7. 反序列化输出参数
        """
        indent = "    " * (namespace_depth + 2)
        inner_indent = "    " * (namespace_depth + 3)
        lines = []

        return_type = method.return_type.base_type
        has_return = return_type != 'void'

        # 构建参数列表
        param_names = [param.name for param in method.parameters]
        params_call = ", ".join(param_names) if param_names else ""

        # ======== 本地对象短路检查 ========
        lines.append(f"{indent}// Check if target is local object")
        lines.append(f"{indent}auto& obj_mgr = GetObjectManager();")
        lines.append(f"{indent}if (obj_mgr.IsLocalObject(GetObjectId())) {{")
        lines.append(f"{indent}    DAS::DasPtr<IDasBase> obj_holder;")
        lines.append(f"{indent}    obj_mgr.LookupObject(GetObjectId(), obj_holder.Put());")
        lines.append(f"{indent}    auto* local_impl = static_cast<{interface.name}*>(obj_holder.Get());")

        # 本地调用
        if has_return:
            lines.append(f"{indent}    return local_impl->{method.name}({params_call});")
        else:
            lines.append(f"{indent}    local_impl->{method.name}({params_call});")
            lines.append(f"{indent}    return DAS_S_OK;")

        lines.append(f"{indent}}}")
        lines.append(f"{indent}")
        lines.append(f"{indent}// Remote object: proceed with IPC")
        lines.append(f"{indent}")

        # ======== IPC 远程调用 ========
        in_params = [p for p in method.parameters if p.direction != ParamDirection.OUT]
        out_params = [p for p in method.parameters if p.direction in (ParamDirection.OUT, ParamDirection.INOUT)]

        need_request_body = bool(in_params)
        need_response_body = bool(out_params) or has_return

        # 判断是否有 [in] 接口指针参数，用于 PendingInParamExportGuard
        has_in_iface = self._has_in_interface_params(method)
        if has_in_iface:
            lines.append(f"{indent}Das::Core::IPC::PendingInParamExportGuard pending_in_param_exports{{GetObjectManager()}};")
            lines.append("")

        # Always generate a V3 Body Header (16 bytes) — even for no-param methods.
        # This ensures ParseV3BodyHeader always gets a valid body on the stub side.
        if self._is_fixed_size_method(method):
            # 零堆分配路径: 栈上 packed struct (works for empty in_params too — just the 16-byte V3 header)
            struct_lines = self._generate_fixed_size_request_body(
                interface, method, method_index, in_params, indent)
            lines.extend(struct_lines)
        else:
            # 变长路径: MemorySerializerWriter with Reserve
            var_lines = self._generate_variable_size_request_body(
                interface, method, method_index, in_params, indent)
            lines.extend(var_lines)

        lines.append("")
        lines.append(f"{indent}std::vector<uint8_t> response_body;")
        is_binary_buffer = method.attributes.get('binary_buffer', False)
        if is_binary_buffer:
            lines.append(f"{indent}uint16_t response_flags = 0;")
        if self._is_fixed_size_method(method):
            if is_binary_buffer:
                lines.append(f"{indent}ipc_result = SendRequest({method_index},")
                lines.append(f"{indent}    reinterpret_cast<const uint8_t*>(&req), sizeof(req), response_body, &response_flags);")
            else:
                lines.append(f"{indent}ipc_result = SendRequest({method_index},")
                lines.append(f"{indent}    reinterpret_cast<const uint8_t*>(&req), sizeof(req), response_body);")
        else:
            lines.append(f"{indent}const std::vector<uint8_t>& request_body = writer.GetBuffer();")
            if is_binary_buffer:
                lines.append(f"{indent}ipc_result = SendRequest({method_index},")
                lines.append(f"{indent}    request_body.data(), request_body.size(), response_body, &response_flags);")
            else:
                lines.append(f"{indent}ipc_result = SendRequest({method_index},")
                lines.append(f"{indent}    request_body.data(), request_body.size(), response_body);")

        # Commit guard: if method has [in] interface params, commit on non-transport errors
        if has_in_iface:
            lines.append(f"{indent}if (!Das::Core::IPC::IsTransportLevelError(ipc_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    pending_in_param_exports.Commit();")
            lines.append(f"{indent}}}")

        lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
        lines.append(f"{indent}{{")
        if has_return:
            lines.append(f"{indent}    return ipc_result;")
        else:
            lines.append(f"{indent}    return;")
        lines.append(f"{indent}}}")
        lines.append("")
        
        if need_response_body:
            lines.append(f"{indent}MemorySerializerReader reader(response_body);")
            lines.append("")
            lines.append(f"{indent}DasResult remote_result;")
            lines.append(f"{indent}ipc_result = reader.ReadInt32(&remote_result);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
            lines.append(f"{indent}{{")
            if has_return:
                lines.append(f"{indent}    return ipc_result;")
            else:
                lines.append(f"{indent}    return;")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}if (DAS::IsFailed(remote_result))")
            lines.append(f"{indent}{{")
            if has_return:
                lines.append(f"{indent}    return remote_result;")
            else:
                lines.append(f"{indent}    return;")
            lines.append(f"{indent}}}")
            lines.append("")
            
            for param in out_params:
                deserialize_code = self._generate_deserialize_param(param, indent, has_return, method)
                for line in deserialize_code:
                    lines.append(f"{line}")
            
            is_das_result_return = return_type in ('DasResult', 'int32_t')
            if has_return and is_das_result_return:
                # DasResult return: remote_result already contains the return value (no redundant field)
                lines.append(f"{indent}return remote_result;")
            elif has_return:
                return_code = self._generate_return_deserialize(method.return_type, indent)
                for line in return_code:
                    lines.append(f"{line}")
            else:
                lines.append(f"{indent}return DAS_S_OK;")
        else:
            lines.append(f"{indent}return DAS_S_OK;")
        
        return "\n".join(lines)
    
    def _generate_serialize_param(self, param: ParameterDef, indent: str) -> List[str]:
        """生成参数序列化代码"""
        lines = []

        # IDasReadOnlyString [in] 参数特殊处理: GetUtf8 + WriteString
        if param.type_info.base_type == "IDasReadOnlyString":
            pn = param.name
            lines.append(f"{indent}// 序列化 IDasReadOnlyString [in] 参数")
            lines.append(f"{indent}const char* {pn}_u8 = nullptr;")
            lines.append(f"{indent}ipc_result = {pn}->GetUtf8(&{pn}_u8);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return ipc_result;")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}ipc_result = writer.WriteString({pn}_u8, std::strlen({pn}_u8));")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return ipc_result;")
            lines.append(f"{indent}}}")
            return lines

        # 使用 type_kind 检测接口类型
        is_iface = (param.type_info.type_kind == TypeKind.INTERFACE
                    and param.type_info.is_pointer)
        if is_iface:
            interface_name = param.type_info.base_type.split("::")[-1]
            param_name = param.name

            # 获取 ObjectId 并序列化（使用 3-arg API + guard Track）
            lines.append(f"{indent}// 序列化接口指针: {interface_name}*")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    Das::Core::IPC::ObjectId {param_name}_id{{}};")
            lines.append(f"{indent}    bool {param_name}_newly_registered = false;")
            lines.append(f"{indent}    ipc_result = Das::Core::IPC::SerializeInInterfaceParam({param_name}, GetObjectManager(), {param_name}_id, &{param_name}_newly_registered);")
            lines.append(f"{indent}    if (DAS::IsFailed(ipc_result))")
            lines.append(f"{indent}    {{")
            lines.append(f"{indent}        return ipc_result;")
            lines.append(f"{indent}    }}")
            lines.append(f"{indent}    pending_in_param_exports.Track({param_name}_id, {param_name}_newly_registered);")
            lines.append(f"{indent}    ipc_result = writer.WriteUInt64(Das::Core::IPC::EncodeObjectId({param_name}_id));")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return ipc_result;")
            lines.append(f"{indent}}}")
            return lines

        type_info = self.type_mapper.get_type_info(param.type_info.base_type)

        if type_info is None:
            lines.append(f"{indent}// TODO: Unknown type {param.type_info.base_type}")
            lines.append(f"{indent}// ipc_result = writer.WriteCustom<{param.type_info.base_type}>({param.name});")
            return lines

        cpp_type, write_method, _, is_struct = type_info

        if is_struct:
            # 如果参数是指针，使用 -> 运算符访问字段
            if param.type_info.is_pointer and param.type_info.pointer_level >= 1:
                access_name = f"{param.name}->"
            else:
                access_name = f"{param.name}."
            struct_serialize = self._generate_struct_serialize(param.type_info.base_type, access_name, indent)
            for line in struct_serialize:
                lines.append(line)
        else:
            # 处理指针和引用
            if param.type_info.is_pointer or param.type_info.is_reference:
                if param.type_info.pointer_level == 1:
                    lines.append(f"{indent}ipc_result = writer.{write_method}(*{param.name});")
                else:
                    lines.append(f"{indent}ipc_result = writer.{write_method}({param.name});")
            else:
                lines.append(f"{indent}ipc_result = writer.{write_method}({param.name});")

        lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return ipc_result;")
        lines.append(f"{indent}}}")

        return lines
    
    def _generate_deserialize_param(self, param: ParameterDef, indent: str, has_return: bool = True, method: MethodDef = None) -> List[str]:
        """生成参数反序列化代码（用于 [out] 和 [inout] 参数）"""
        lines = []

        # IDasReadOnlyString special handling: ReadString + CreateIDasReadOnlyStringFromUtf8
        if param.type_info.base_type == "IDasReadOnlyString" and param.type_info.is_pointer:
            pn = param.name
            lines.append(f"{indent}// 反序列化 IDasReadOnlyString")
            lines.append(f"{indent}std::string {pn}_str;")
            lines.append(f"{indent}ipc_result = reader.ReadString({pn}_str);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
            lines.append(f"{indent}{{")
            if has_return:
                lines.append(f"{indent}    return ipc_result;")
            else:
                lines.append(f"{indent}    return;")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}ipc_result = ::CreateIDasReadOnlyStringFromUtf8({pn}_str.c_str(), {pn});")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
            lines.append(f"{indent}{{")
            if has_return:
                lines.append(f"{indent}    return ipc_result;")
            else:
                lines.append(f"{indent}    return;")
            lines.append(f"{indent}}}")
            return lines

        type_info = self.type_mapper.get_type_info(param.type_info.base_type)

        # Check if this is an interface pointer type using type_kind
        is_interface = (param.type_info.type_kind == TypeKind.INTERFACE
                        and param.type_info.is_pointer)

        if is_interface and param.type_info.is_pointer:
            # Get the interface namespace for fully qualified name
            interface_name = self.type_mapper.get_interface_name(param.type_info.base_type)
            interface_ns = self.type_mapper.interface_namespaces.get(param.type_info.base_type, "")
            if interface_ns:
                full_interface_name = f"{interface_ns}::{interface_name}"
            else:
                full_interface_name = interface_name

            pn = param.name
            lines.append(f"{indent}// 反序列化 [out] 接口指针: {interface_name}*")
            lines.append(f"{indent}uint16_t {pn}_session_id = 0;")
            lines.append(f"{indent}uint16_t {pn}_generation = 0;")
            lines.append(f"{indent}uint32_t {pn}_local_id = 0;")
            lines.append(f"{indent}uint32_t {pn}_interface_id = 0;")
            lines.append(f"{indent}ipc_result = reader.ReadUInt16(&{pn}_session_id);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result)) {{ return ipc_result; }}")
            lines.append(f"{indent}ipc_result = reader.ReadUInt16(&{pn}_generation);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result)) {{ return ipc_result; }}")
            lines.append(f"{indent}ipc_result = reader.ReadUInt32(&{pn}_local_id);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result)) {{ return ipc_result; }}")
            lines.append(f"{indent}ipc_result = reader.ReadUInt32(&{pn}_interface_id);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result)) {{ return ipc_result; }}")
            lines.append(f"{indent}if ({pn}_session_id != 0 || {pn}_local_id != 0)")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    Das::Core::IPC::ObjectId {pn}_oid{{")
            lines.append(f"{indent}        .session_id = {pn}_session_id,")
            lines.append(f"{indent}        .generation = {pn}_generation,")
            lines.append(f"{indent}        .local_id   = {pn}_local_id}};")
            lines.append(f"{indent}    GetObjectManager().RegisterRemoteObject({pn}_oid);")
            lines.append(f"{indent}    IDasBase* {pn}_base = Das::Core::IPC::DasIpcProxy::CreateProxyByInterfaceId(")
            lines.append(f"{indent}        {pn}_interface_id, {pn}_oid, *GetRunLoop(), GetBusinessThread(), GetObjectManager());")
            lines.append(f"{indent}    *{pn} = static_cast<{full_interface_name}*>({pn}_base);")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}else")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    *{pn} = nullptr;")
            lines.append(f"{indent}}}")
            return lines

        # Binary buffer [out] deserialization
        if method and method.attributes.get('binary_buffer', False):
            bt = param.type_info.base_type
            if bt in ('unsigned char', 'uint8_t') and param.type_info.is_pointer:
                pn = param.name
                lines.append(f"{indent}// Check if response uses SHM path")
                lines.append(f"{indent}if (response_flags & Das::Core::IPC::MessageFlags::SHM_RESPONSE)")
                lines.append(f"{indent}{{")
                lines.append(f"{indent}    // SHM path: read handle + size, copy from shared memory")
                lines.append(f"{indent}    uint64_t shm_handle = 0;")
                lines.append(f"{indent}    uint64_t shm_data_size = 0;")
                lines.append(f"{indent}    ipc_result = reader.ReadUInt64(&shm_handle);")
                lines.append(f"{indent}    if (DAS::IsFailed(ipc_result))")
                lines.append(f"{indent}    {{")
                if has_return:
                    lines.append(f"{indent}        return ipc_result;")
                else:
                    lines.append(f"{indent}        return;")
                lines.append(f"{indent}    }}")
                lines.append(f"{indent}    ipc_result = reader.ReadUInt64(&shm_data_size);")
                lines.append(f"{indent}    if (DAS::IsFailed(ipc_result))")
                lines.append(f"{indent}    {{")
                if has_return:
                    lines.append(f"{indent}        return ipc_result;")
                else:
                    lines.append(f"{indent}        return;")
                lines.append(f"{indent}    }}")
                lines.append(f"{indent}    auto* conn_mgr = GetRunLoop()->GetConnectionManager();")
                lines.append(f"{indent}    if (conn_mgr)")
                lines.append(f"{indent}    {{")
                lines.append(f"{indent}        Das::Core::IPC::ConnectionInfo conn_info;")
                lines.append(f"{indent}        if (DAS::IsOk(conn_mgr->GetConnection(GetSourceSessionId(), conn_info))")
                lines.append(f"{indent}            && conn_info.shm_pool != nullptr)")
                lines.append(f"{indent}        {{")
                lines.append(f"{indent}            Das::Core::IPC::SharedMemoryBlock shm_block;")
                lines.append(f"{indent}            DasResult shm_result = conn_info.shm_pool->GetBlockByHandle(shm_handle, shm_block);")
                lines.append(f"{indent}            if (DAS::IsFailed(shm_result))")
                lines.append(f"{indent}            {{")
                lines.append(f'{indent}                DAS_CORE_LOG_ERROR("Proxy: GetBlockByHandle failed for handle={{}}, result={{}}", shm_handle, shm_result);')
                if has_return:
                    lines.append(f"{indent}                return DAS_E_IPC_SHM_FAILED;")
                else:
                    lines.append(f"{indent}                return;")
                lines.append(f"{indent}            }}")
                lines.append(f"{indent}            data_cache_.resize(static_cast<size_t>(shm_data_size));")
                lines.append(f"{indent}            std::memcpy(data_cache_.data(), shm_block.data, static_cast<size_t>(shm_data_size));")
                lines.append(f"{indent}            // Release SHM block after successful read")
                lines.append(f"{indent}            Das::Core::IPC::ReleaseShmBlockPayload release_payload;")
                lines.append(f"{indent}            release_payload.shm_handle = shm_handle;")
                lines.append(f"{indent}            release_payload.source_session_id = GetSourceSessionId();")
                lines.append(f"{indent}            std::vector<uint8_t> release_body(")
                lines.append(f"{indent}                reinterpret_cast<const uint8_t*>(&release_payload),")
                lines.append(f"{indent}                reinterpret_cast<const uint8_t*>(&release_payload) + sizeof(release_payload));")
                lines.append(f"{indent}            auto release_header =")
                lines.append(f"{indent}                Das::Core::IPC::IPCMessageHeaderBuilder()")
                lines.append(f"{indent}                    .SetMessageType(Das::Core::IPC::MessageType::EVENT)")
                lines.append(f"{indent}                    .SetHeaderFlags(Das::Core::IPC::HeaderFlags::CONTROL_PLANE)")
                lines.append(f"{indent}                    .SetInterfaceId(static_cast<uint32_t>(Das::Core::IPC::IpcCommandType::RELEASE_SHM_BLOCK))")
                lines.append(f"{indent}                    .SetSourceSessionId(GetSourceSessionId())")
                lines.append(f"{indent}                    .SetTargetSessionId(GetObjectId().session_id)")
                lines.append(f"{indent}                    .Build();")
                lines.append(f"{indent}            GetRunLoop()->PostSend(release_header, std::move(release_body));")
                lines.append(f"{indent}        }}")
                lines.append(f"{indent}    }}")
                lines.append(f"{indent}    *{pn} = data_cache_.data();")
                lines.append(f"{indent}}}")
                lines.append(f"{indent}else")
                lines.append(f"{indent}{{")
                lines.append(f"{indent}    // Pipe path: read binary data directly")
                lines.append(f"{indent}    ipc_result = reader.ReadBytes(data_cache_);")
                lines.append(f"{indent}    if (DAS::IsFailed(ipc_result))")
                lines.append(f"{indent}    {{")
                if has_return:
                    lines.append(f"{indent}        return ipc_result;")
                else:
                    lines.append(f"{indent}        return;")
                lines.append(f"{indent}    }}")
                lines.append(f"{indent}    *{pn} = data_cache_.data();")
                lines.append(f"{indent}}}")
                return lines

        if type_info is None:
            lines.append(f"{indent}// TODO: Unknown type {param.type_info.base_type}")
            lines.append(f"{indent}// ipc_result = reader.ReadCustom<{param.type_info.base_type}>({param.name});")
            return lines
        
        cpp_type, _, read_method, is_struct = type_info
        
        if is_struct:
            # 如果参数是指针，使用 -> 运算符访问字段
            if param.type_info.is_pointer and param.type_info.pointer_level >= 1:
                access_name = f"{param.name}->"
            else:
                access_name = f"{param.name}."
            struct_deserialize = self._generate_struct_deserialize(param.type_info.base_type, access_name, indent, has_return)
            for line in struct_deserialize:
                lines.append(line)
        else:
            if param.type_info.is_pointer and param.type_info.pointer_level >= 1:
                # Enum types: read into temp int32_t then cast to enum pointer
                if param.type_info.base_type in self.type_mapper.enum_types:
                    lines.append(f"{indent}int32_t {param.name}_temp;")
                    lines.append(f"{indent}ipc_result = reader.{read_method}(&{param.name}_temp);")
                    lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
                    lines.append(f"{indent}{{")
                    if has_return:
                        lines.append(f"{indent}    return ipc_result;")
                    else:
                        lines.append(f"{indent}    return;")
                    lines.append(f"{indent}}}")
                    lines.append(f"{indent}*{param.name} = static_cast<{param.type_info.base_type}>({param.name}_temp);")
                else:
                    lines.append(f"{indent}ipc_result = reader.{read_method}({param.name});")
            else:
                lines.append(f"{indent}ipc_result = reader.{read_method}(&{param.name});")
        
        lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
        lines.append(f"{indent}{{")
        if has_return:
            lines.append(f"{indent}    return ipc_result;")
        else:
            lines.append(f"{indent}    return;")
        lines.append(f"{indent}}}")
        
        return lines
    
    def _generate_return_deserialize(self, return_type: TypeInfo, indent: str) -> List[str]:
        """生成返回值反序列化代码"""
        lines = []
        type_info = self.type_mapper.get_type_info(return_type.base_type)
        
        if type_info is None:
            lines.append(f"{indent}// TODO: Deserialize return value of type {return_type.base_type}")
            lines.append(f"{indent}return DAS_E_NOT_IMPLEMENTED;")
            return lines
        
        cpp_type, _, read_method, is_struct = type_info
        
        if is_struct:
            lines.append(f"{indent}{return_type.base_type} ret_value;")
            struct_deserialize = self._generate_struct_deserialize(return_type.base_type, "ret_value", indent, True)
            for line in struct_deserialize:
                lines.append(line)
            lines.append(f"{indent}return ret_value;")
        else:
            lines.append(f"{indent}{cpp_type} ret_value;")
            lines.append(f"{indent}ipc_result = reader.{read_method}(&ret_value);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return ipc_result;")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}return ret_value;")
        
        return lines

    def _generate_struct_serialize(self, struct_name: str, param_access: str, indent: str) -> List[str]:
        """生成 struct 的内联序列化代码（展开每个字段）

        Args:
            param_access: 参数访问表达式（如 "params." 或 "params->"）
        """
        lines = []
        struct_def = self.type_mapper.struct_defs.get(struct_name)
        if struct_def is None:
            lines.append(f"{indent}// TODO: Unknown struct {struct_name}")
            return lines

        for field in struct_def.fields:
            type_info = self.type_mapper.get_type_info(field.type_name)
            if type_info is None or type_info[3]:
                lines.append(f"{indent}// TODO: Unsupported field type {field.type_name}")
                continue

            _, write_method, _, _ = type_info
            lines.append(f"{indent}ipc_result = writer.{write_method}({param_access}{field.name});")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return ipc_result;")
            lines.append(f"{indent}}}")

        return lines

    def _generate_struct_deserialize(self, struct_name: str, param_access: str, indent: str, has_return: bool = True) -> List[str]:
        """生成 struct 的内联反序列化代码（展开每个字段）

        Args:
            param_access: 参数访问表达式（如 "ret_value." 或 "p_out_params->"）
        """
        lines = []
        struct_def = self.type_mapper.struct_defs.get(struct_name)
        if struct_def is None:
            lines.append(f"{indent}// TODO: Unknown struct {struct_name}")
            return lines

        for field in struct_def.fields:
            type_info = self.type_mapper.get_type_info(field.type_name)
            if type_info is None or type_info[3]:
                lines.append(f"{indent}// TODO: Unsupported field type {field.type_name}")
                continue

            _, _, read_method, _ = type_info
            lines.append(f"{indent}ipc_result = reader.{read_method}(&{param_access}{field.name});")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result))")
            lines.append(f"{indent}{{")
            if has_return:
                lines.append(f"{indent}    return ipc_result;")
            else:
                lines.append(f"{indent}    return;")
            lines.append(f"{indent}}}")

        return lines

    def _generate_proxy_class_old(self, interface: InterfaceDef, namespace_depth: int = 0) -> str:
        """为接口生成混合模式 Proxy 类 (B7)
        
        模板参数:
        - IsLocal=true: 本地短路模式，直接调用实现
        - IsLocal=false: IPC 远程模式，序列化调用
        """
        lines = []
        indent = "    " * namespace_depth
        class_indent = "    " * (namespace_depth + 1)
        method_indent = "    " * (namespace_depth + 2)
        
        interface_short_name = self._get_interface_short_name(interface.name)
        class_name = f"{interface_short_name}Proxy"
        interface_id = fnv1a_hash_guid(interface.uuid)
        
        # 类文档注释
        lines.append(f"{indent}// ============================================================================")
        lines.append(f"{indent}// {class_name}<IsLocal>")
        lines.append(f"{indent}// Hybrid IPC Proxy for {interface.name}")
        lines.append(f"{indent}// Interface UUID: {interface.uuid}")
        lines.append(f"{indent}// ============================================================================")
        lines.append("")
        
        # 模板类定义
        lines.append(f"{indent}template <bool IsLocal>")
        lines.append(f"{indent}class {class_name}")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}public:")
        
        # InterfaceId 常量
        lines.append(f"{class_indent}static constexpr uint32_t InterfaceId = 0x{interface_id:08X}u;")
        lines.append("")
        
        # MethodTable 常量
        if interface.methods:
            lines.append(f"{class_indent}static constexpr MethodMetadata MethodTable[] = {{")
            for i, method in enumerate(interface.methods):
                method_hash = fnv1a_hash(f"{interface.name}::{method.name}")
                lines.append(f"{method_indent}{{ /* method_id */ {i}, /* name */ \"{method.name}\", /* hash */ 0x{method_hash:08X}u }},")
            lines.append(f"{class_indent}}};")
        else:
            lines.append(f"{class_indent}static constexpr MethodMetadata MethodTable[] = {{{{}}}};")
        lines.append("")
        
        # 构造函数 - IPC 模式
        lines.append(f"{class_indent}// Constructor for IPC mode (IsLocal=false)")
        lines.append(f"{class_indent}{class_name}(IpcTransport& transport, ObjectId target)")
        lines.append(f"{class_indent}    : transport_(transport)")
        lines.append(f"{class_indent}    , target_(target)")
        lines.append(f"{class_indent}    , next_call_id_(1)")
        lines.append(f"{class_indent}    , local_impl_(nullptr)")
        lines.append(f"{class_indent}{{")
        lines.append(f"{class_indent}}}")
        lines.append("")
        
        # 构造函数 - 本地模式
        lines.append(f"{class_indent}// Constructor for local mode (IsLocal=true)")
        lines.append(f"{class_indent}{class_name}({interface.name}* local_impl)")
        lines.append(f"{class_indent}    : transport_(*reinterpret_cast<IpcTransport*>(0))")
        lines.append(f"{class_indent}    , target_()")
        lines.append(f"{class_indent}    , next_call_id_(1)")
        lines.append(f"{class_indent}    , local_impl_(local_impl)")
        lines.append(f"{class_indent}{{")
        lines.append(f"{class_indent}}}")
        lines.append("")
        
        # 方法声明和实现
        for i, method in enumerate(interface.methods):
            method_sig = self._generate_method_signature(interface, method, interface.namespace)
            lines.append(f"{class_indent}{method_sig} override")
            lines.append(f"{class_indent}{{")
            
            # 方法体
            method_body = self._generate_method_body(interface, method, i, namespace_depth)
            for line in method_body.splitlines():
                lines.append(f"{line}")
            
            lines.append(f"{class_indent}}}")
            lines.append("")
        
        # 私有成员
        lines.append(f"{indent}private:")
        lines.append(f"{class_indent}IpcTransport& transport_;")
        lines.append(f"{class_indent}ObjectId target_;")
        lines.append(f"{class_indent}uint32_t next_call_id_;")
        lines.append(f"{class_indent}[[maybe_unused]] {interface.name}* local_impl_;")
        lines.append(f"{indent}}};")
        lines.append("")
        
        # 类型别名
        lines.append(f"{indent}// Type aliases for convenience")
        lines.append(f"{indent}using {interface_short_name}ProxyLocal = {class_name}<true>;")
        lines.append(f"{indent}using {interface_short_name}ProxyRemote = {class_name}<false>;")
        lines.append("")
        
        return "\n".join(lines)
    
    def _generate_interface_json(self, interface: InterfaceDef) -> Dict[str, Any]:
        """生成接口元数据 JSON"""
        interface_id = fnv1a_hash_guid(interface.uuid)
        methods = []
        for i, method in enumerate(interface.methods):
            method_hash = fnv1a_hash(f"{interface.name}::{method.name}")
            methods.append({
                "method_id": i,
                "name": method.name,
                "hash": f"0x{method_hash:08X}"
            })
        
        return {
            "interface_name": interface.name,
            "interface_id": f"0x{interface_id:08X}",
            "uuid": interface.uuid,
            "namespace": interface.namespace or "",
            "base_interface": interface.base_interface or "",
            "methods": methods
        }
    
    def generate_proxy_headers(self, output_dir: str, cache_dir: Optional[str] = None) -> List[str]:
        """生成所有接口的 IPC Proxy 头文件
        
        Args:
            output_dir: 输出目录
            cache_dir: 缓存目录（用于写入 interface.json）
            
        Returns:
            生成的文件路径列表
        """
        generated_files = []

        proxy_dir = os.path.join(output_dir, "proxy")
        os.makedirs(proxy_dir, exist_ok=True)

        if cache_dir:
            os.makedirs(cache_dir, exist_ok=True)

        # 尝试从 ABI 目录加载外部接口的命名空间映射
        abi_dir = os.path.join(os.path.dirname(output_dir), "abi")
        self.type_mapper.load_namespaces_from_abi_dir(abi_dir)
        
        for interface in self.document.interfaces:
            interface_short_name = self._get_interface_short_name(interface.name)
            filename = f"{interface_short_name}Proxy.h"
            filepath = os.path.join(proxy_dir, filename)
            
            ns_prefix = ""
            if interface.namespace:
                ns_prefix = interface.namespace.replace("::", "_") + "_"
            guard_name = f"DAS_IPC_{ns_prefix}{interface_short_name.upper()}_PROXY_H"
            
            content = self._file_header(guard_name, interface.name, interface)
            
            ns_depth = 0
            if interface.namespace:
                content += self._generate_namespace_open(interface.namespace)
                ns_depth = len(interface.namespace.split("::"))

                ns_indent = "    " * ns_depth
                ns_indent_inner = "    " * (ns_depth + 1)
                content += f"{ns_indent}namespace IPC\n"
                content += f"{ns_indent}{{\n"
                content += f"{ns_indent_inner}namespace Proxy\n"
                content += f"{ns_indent_inner}{{\n"
                content += f"{ns_indent_inner}using namespace Das::Core::IPC;\n"
                ns_depth += 2
            else:
                content += "using namespace Das::Core::IPC;\n"
            
            content += self._generate_proxy_class(interface, ns_depth)
            
            if interface.namespace:
                ns_indent = "    " * (ns_depth - 2)
                ns_indent_inner = "    " * (ns_depth - 1)
                content += f"{ns_indent_inner}}}\n"
                content += f"{ns_indent_inner}// namespace Proxy\n"
                content += f"{ns_indent}}}\n"
                content += f"{ns_indent}// namespace IPC\n"
                content += self._generate_namespace_close(interface.namespace)
            
            content += self._file_footer(guard_name)
            
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(content)
            
            print(f"Generated: {filepath}")
            generated_files.append(filepath)
            
            if cache_dir:
                json_data = self._generate_interface_json(interface)
                json_filename = f"{interface_short_name}.json"
                json_filepath = os.path.join(cache_dir, json_filename)
                with open(json_filepath, 'w', encoding='utf-8') as f:
                    json.dump(json_data, f, indent=2)
                print(f"Generated: {json_filepath}")

        return generated_files

    def _generate_proxy_factory(self) -> str:
        """生成 CreateProxyByInterfaceId 映射函数"""
        lines = []
        lines.append("")
        lines.append("// CreateProxyByInterfaceId 映射函数（由 IDL 生成器生成）")
        lines.append("namespace DasIpcProxy {")
        lines.append("")
        lines.append("template <typename TProxy>")
        lines.append("IDasBase* CreateTypedProxy(")
        lines.append("    const ObjectId& object_id,")
        lines.append("    IpcRunLoop& run_loop,")
        lines.append("    std::weak_ptr<BusinessThread> business_thread,")
        lines.append("    DistributedObjectManager& object_manager)")
        lines.append("{")
        lines.append("    return new TProxy(object_id, run_loop, std::move(business_thread), object_manager);")
        lines.append("}")
        lines.append("")
        lines.append("inline IDasBase* CreateProxyByInterfaceId(")
        lines.append("    uint32_t interface_id,")
        lines.append("    const ObjectId& object_id,")
        lines.append("    IpcRunLoop& run_loop,")
        lines.append("    std::weak_ptr<BusinessThread> business_thread,")
        lines.append("    DistributedObjectManager& object_manager)")
        lines.append("{")
        lines.append("    switch (interface_id)")
        lines.append("    {")

        for interface in self.document.interfaces:
            interface_id = fnv1a_hash_guid(interface.uuid)
            proxy_name = f"{interface.name}Proxy"
            lines.append(f"        case 0x{interface_id:08X}: // {interface.name}::InterfaceId")
            lines.append("        {")
            lines.append(f"            return CreateTypedProxy<{proxy_name}>(object_id, run_loop, business_thread, object_manager);")
            lines.append("        }")

        lines.append("        default:")
        lines.append("        {")
        lines.append('            DAS_CORE_LOG_ERROR("Unknown interface_id: 0x{:08X}", interface_id);')
        lines.append("            return nullptr;")
        lines.append("        }")
        lines.append("    }")
        lines.append("}")
        lines.append("")
        lines.append("} // namespace DasIpcProxy")

        return "\n".join(lines)

    def generate_proxy_factory(self, output_dir: str) -> str:
        """生成 CreateProxyByInterfaceId 映射函数到单独文件"""
        factory_content = """// This file is automatically generated by DAS IDL Generator
// !!! DO NOT EDIT !!!
#ifndef DAS_IPC_PROXY_FACTORY_H
#define DAS_IPC_PROXY_FACTORY_H

#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/IDasBase.h>

"""
        factory_content += self._generate_proxy_factory()
        factory_content += """

#endif // DAS_IPC_PROXY_FACTORY_H
"""
        filepath = os.path.join(output_dir, "IpcProxyFactory.h")
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(factory_content)
        print(f"Generated: {filepath}")
        return filepath


def generate_ipc_proxy_files(
    document: IdlDocument,
    output_dir: str,
    base_name: Optional[str] = None,
    idl_file_path: Optional[str] = None,
    cache_dir: Optional[str] = None
) -> List[str]:
    """生成 IPC Proxy 文件
    
    Args:
        document: IDL 文档对象
        output_dir: 输出目录
        base_name: 基础文件名（可选，默认使用 IDL 文件名）
        idl_file_path: IDL 文件路径（可选）
        cache_dir: 缓存目录（用于写入 interface.json）
        
    Returns:
        生成的文件路径列表
    """
    generator = IpcProxyGenerator(document, idl_file_path)
    return generator.generate_proxy_headers(output_dir, cache_dir)


# 测试代码
if __name__ == '__main__':
    parse_idl = _das_idl_parser.parse_idl
    
    test_idl = '''
    import "DasBasicTypes.idl";

    namespace Das::ExportInterface {

    [uuid("9BC34D72-E442-4944-ACE6-69257D262568")]
    interface IDasSourceLocation {
        [get, set] IDasReadOnlyString FileName;
        [get, set] int32_t Line;
        [get, set] IDasReadOnlyString FunctionName;
    };

    [uuid("F4191604-D061-49A4-85EC-28EFC376119F")]
    interface IDasLogReader : IDasBase {
        DasResult ReadOne(IDasReadOnlyString* message);
    }

    [uuid("806E244C-CCF0-4DC3-AD54-6886FDF9B1F4")]
    interface IDasLogRequester : IDasBase {
        DasResult RequestOne(IDasLogReader* p_reader);
    }

    }
    '''

    doc = parse_idl(test_idl)
    generator = IpcProxyGenerator(doc, "test.idl")
    
    print("=== 生成的 IPC Proxy 头文件 ===")
    files = generator.generate_proxy_headers("./test_output")
    for f in files:
        print(f"  - {f}")
