"""
DAS IPC Stub 代码生成器

从 IDL 接口定义生成 IPC Stub 代码，用于服务端接收和处理 IPC 调用。

功能:
1. 为每个接口生成 Stub 类，继承 IStubBase
2. 使用 FNV-1a hash 计算 InterfaceId（基于 UUID，与 Proxy 一致）
3. 生成 MethodTable 元数据（与 Proxy 一致）
4. 实现 Dispatch 方法，根据 method_id 分发到具体处理函数
5. 写入 interface.json 到 cache_dir

输出文件命名: stub/<InterfaceName>Stub.h
"""

import json
import os
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional
import importlib
import sys

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
ParamDirection = _das_idl_parser.ParamDirection
PropertyDef = _das_idl_parser.PropertyDef
parse_idl_file = _das_idl_parser.parse_idl_file


class StubTypeMapper:
    """Stub 类型映射器
    
    将 IDL 类型映射到序列化方法：
    1. 基本类型 → SerializerWriter/SerializerReader 方法
    2. Struct 类型 → Serialize_/Deserialize_ 函数调用
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
            # struct 类型使用 Serialize_/Deserialize_ 函数
            return (idl_type, None, None, True)
        
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
        import re as re_module
        abi_path = Path(abi_dir)
        if not abi_path.is_dir():
            return

        # 正则表达式匹配 namespace 块中的 DAS_INTERFACE 前向声明
        ns_block_pattern = re_module.compile(
            r'namespace\s+([\w:]+)\s*\{',
            re_module.DOTALL
        )
        iface_decl_pattern = re_module.compile(r'DAS_INTERFACE\s+(\w+)\s*;')

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

        # 添加已知的接口头文件映射回退（多个接口可能在同一个头文件中）
        # 这些接口在同一个 IDL 文件中定义但共享一个 ABI 头文件
        fallback_mappings = {
            "IDasWeakReferenceSource": "IDasWeakReference.h",
        }
        for iface_name, header_name in fallback_mappings.items():
            if iface_name not in self.interface_header_files:
                self.interface_header_files[iface_name] = header_name


def fnv1a_hash(data: str) -> int:
    """计算字符串的 FNV-1a 32-bit hash"""
    FNV_PRIME = 0x01000193
    FNV_OFFSET_BASIS = 0x811c9dc5
    
    hash_value = FNV_OFFSET_BASIS
    for char in data.encode('utf-8'):
        hash_value ^= char
        hash_value = (hash_value * FNV_PRIME) & 0xFFFFFFFF
    
    return hash_value


def fnv1a_hash_guid(guid_str: str) -> int:
    """计算 GUID 字符串的 FNV-1a 32-bit hash（用于生成 interface_id）
    
    处理两种格式，大小写不敏感
    """
    FNV_PRIME = 0x01000193
    FNV_OFFSET_BASIS = 0x811c9dc5
    
    hash_value = FNV_OFFSET_BASIS
    for char in guid_str:
        if char == '{' or char == '}':
            continue
        upper_char = char.upper() if 'a' <= char <= 'z' else char
        hash_value ^= ord(upper_char)
        hash_value = (hash_value * FNV_PRIME) & 0xFFFFFFFF
    
    return hash_value


class IpcStubGenerator:
    """IPC Stub 代码生成器"""
    
    def __init__(self, document: IdlDocument, idl_file_path: Optional[str] = None):
        self.document = document
        self.idl_file_path = idl_file_path
        self.idl_file_name = os.path.basename(idl_file_path) if idl_file_path else None
        self.indent = "    "  # 4 空格缩进
        self.type_mapper = StubTypeMapper(document)

    @staticmethod
    def _properties_to_methods(properties: list) -> list:
        """将 PropertyDef 列表转换为 MethodDef 列表（getter/setter）"""
        methods = []
        for prop in properties:
            is_interface = prop.type_info.base_type.startswith('I') and prop.type_info.pointer_level == 0
            is_string = prop.type_info.base_type in ('string', 'IDasReadOnlyString')

            if prop.has_getter:
                if is_interface or is_string:
                    out_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=2, is_pointer=True)
                    params = [ParameterDef(name="pp_out", type_info=out_type, direction=ParamDirection.OUT)]
                else:
                    out_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=1, is_pointer=True)
                    params = [ParameterDef(name="p_out", type_info=out_type, direction=ParamDirection.OUT)]
                methods.append(MethodDef(name=f"Get{prop.name}", return_type=TypeInfo(base_type="DasResult", pointer_level=0), parameters=params))

            if prop.has_setter:
                if is_interface:
                    in_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=1, is_pointer=True)
                elif is_string:
                    in_type = TypeInfo(base_type="IDasReadOnlyString", pointer_level=1, is_pointer=True)
                else:
                    in_type = TypeInfo(base_type=prop.type_info.base_type, pointer_level=0)
                params = [ParameterDef(name="p_value", type_info=in_type, direction=ParamDirection.IN)]
                methods.append(MethodDef(name=f"Set{prop.name}", return_type=TypeInfo(base_type="DasResult", pointer_level=0), parameters=params))

        return methods

    def _collect_all_methods(self, interface: InterfaceDef) -> list:
        """收集接口及其所有父接口的方法（深度优先，从根到叶）"""
        iface_map = {iface.name: iface for iface in self.document.interfaces}

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

        chain = []
        current = interface
        visited = set()
        while current and current.name != "IDasBase" and current.name not in visited:
            chain.append(current)
            visited.add(current.name)
            current = iface_map.get(current.base_interface)
        chain.reverse()

        all_methods = []
        for iface in chain:
            for method in iface.methods:
                all_methods.append((method, iface))
            for method in self._properties_to_methods(iface.properties):
                all_methods.append((method, iface))
        return all_methods
    
    def _file_header(self, guard_name: str, interface_name: str, abi_header: str = "", interface_includes: List[str] = None) -> str:
        """生成文件头"""
        timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

        idl_file_comment = ""
        if self.idl_file_name:
            idl_file_comment = f"// Source IDL file: {self.idl_file_name}\n"

        includes = []
        includes.append("#include <das/DasTypes.hpp>")
        includes.append("#include <das/Core/IPC/IStubBase.h>")
        includes.append("#include <das/Core/IPC/MemorySerializer.h>")
        includes.append("#include <das/Core/IPC/DistributedObjectManager.h>")
        includes.append("#include <das/Core/IPC/ProxyFactory.h>")
        includes.append("#include <das/Core/IPC/Serializer.h>")
        includes.append("#include <algorithm>")
        includes.append("#include <cstdint>")
        includes.append("#include <cstring>")
        includes.append("#include <string>")
        includes.append("#include <vector>")

        # 添加 ABI 头文件
        if abi_header:
            includes.append(f"#include <{abi_header}>")

        # 添加接口类型 includes
        if interface_includes:
            for inc in interface_includes:
                includes.append(f"#include <{inc}>")

        return f"""#if !defined({guard_name})
#define {guard_name}

// This file is automatically generated by DAS IPC Stub Generator
// Generated at: {timestamp}
{idl_file_comment}// !!! DO NOT EDIT !!!
//
// IPC Stub for {interface_name}
//

{chr(10).join(includes)}

"""
    
    def _collect_interface_includes(self, interface: InterfaceDef) -> List[str]:
        """收集接口方法中使用的接口类型的头文件路径

        遍历所有方法，检查返回类型和参数类型是否为接口类型，
        如果是则收集对应的头文件路径。
        """
        # 核心类型（不需要额外 include）
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
    
    def _get_cpp_type(self, type_info: TypeInfo) -> str:
        """将 IDL 类型转换为 C++ 类型"""
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
        
        result = ""
        if type_info.is_const:
            result += "const "
        
        result += base
        
        if type_info.is_pointer:
            result += "*" * type_info.pointer_level
        
        if type_info.is_reference:
            result += "&"
        
        return result

    def _get_cpp_type_base(self, type_info: TypeInfo) -> str:
        """获取基础 C++ 类型（不含指针级别），用于 [out] 接口参数声明

        对于 [out] 接口参数，IDL 层面是 IDasXxx*，ABI 层面是 IDasXxx**
        这里返回 IDL 层面的类型（单指针），由调用方添加 & 来匹配 ABI
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

        result = ""
        if type_info.is_const:
            result += "const "

        result += base

        # 接口类型始终返回单指针（IDL 层面）
        # 需要构造带 * 的完整类型名来检测接口类型
        if type_info.is_pointer:
            full_type = type_info.base_type + "*"
            if self.type_mapper.is_interface_type(full_type):
                result += "*"

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
    
    def _generate_method_constants(self, all_methods: list, namespace_depth: int = 0) -> str:
        """生成 Method ID 常量定义"""
        lines = []
        indent = "    " * (namespace_depth + 1)

        for i, (method, _) in enumerate(all_methods):
            lines.append(f"{indent}static constexpr uint16_t METHOD_{method.name.upper()} = {i};")

        return "\n".join(lines)
    
    def _generate_dispatch_method(self, interface: InterfaceDef, all_methods: list, namespace_depth: int = 0) -> str:
        """生成 DispatchMethod 实例方法（使用 switch case 跳转）"""
        lines = []
        indent = "    " * (namespace_depth + 1)
        inner_indent = "    " * (namespace_depth + 2)

        lines.append(f"{indent}DasResult DispatchMethod(")
        lines.append(f"{indent}    uint16_t method_id,")
        lines.append(f"{indent}    void* impl,")
        lines.append(f"{indent}    const uint8_t* params,")
        lines.append(f"{indent}    size_t params_size,")
        lines.append(f"{indent}    DistributedObjectManager& object_manager,")
        lines.append(f"{indent}    std::vector<uint8_t>& out_response) override")
        lines.append(f"{indent}{{")
        lines.append(f"{inner_indent}auto* target = static_cast<{interface.name}*>(impl);")
        lines.append(f"{inner_indent}if (!target) {{ return DAS_E_INVALID_POINTER; }}")
        lines.append(f"{inner_indent}switch (method_id)")
        lines.append(f"{inner_indent}{{")

        for method, _ in all_methods:
            lines.append(f"{inner_indent}case METHOD_{method.name.upper()}:")
            lines.append(f"{inner_indent}    return Handle{method.name}(target, params, params_size, object_manager, out_response);")

        lines.append(f"{inner_indent}default:")
        lines.append(f"{inner_indent}    return DAS_E_IPC_UNKNOWN_METHOD;")
        lines.append(f"{inner_indent}}}")
        lines.append(f"{indent}}}")

        return "\n".join(lines)
    
    def _generate_handle_method_declaration(self, interface: InterfaceDef, method: MethodDef, namespace_depth: int = 0) -> str:
        """生成 Handle 方法声明（实例方法）"""
        indent = "    " * (namespace_depth + 1)
        return f"{indent}DasResult Handle{method.name}({interface.name}* impl, const uint8_t* params, size_t params_size, DistributedObjectManager& object_manager, std::vector<uint8_t>& out_response);"
    
    def _generate_handle_method_definition(self, interface: InterfaceDef, method: MethodDef, method_index: int) -> str:
        """生成单个方法的 Handle 处理函数定义（实例方法，impl 通过参数传入）"""
        lines = []
        interface_short_name = self._get_interface_short_name(interface.name)
        class_name = f"{interface_short_name}Stub"

        ns_parts = []
        if interface.namespace:
            ns_parts = interface.namespace.split("::")
            full_ns = ns_parts + ["IPC", "Stub"]
        else:
            # 对于没有命名空间的接口，使用 DasIpc::Stub
            full_ns = ["DasIpc", "Stub"]

        inner_indent = "    "

        full_class_name = "::".join(full_ns + [class_name])
        lines.append(f"DasResult {full_class_name}::Handle{method.name}({interface.name}* impl, const uint8_t* params, size_t params_size, DistributedObjectManager& object_manager, std::vector<uint8_t>& out_response)")
        lines.append(f"{{")
        lines.append(f"{inner_indent}(void)impl;")
        lines.append(f"{inner_indent}(void)params;")
        lines.append(f"{inner_indent}(void)params_size;")
        lines.append(f"{inner_indent}(void)object_manager;")
        lines.append(f"{inner_indent}(void)out_response;")

        return_type = method.return_type.base_type
        has_return = return_type != 'void'

        in_params = [p for p in method.parameters if p.direction != ParamDirection.OUT]
        out_params = [p for p in method.parameters if p.direction in (ParamDirection.OUT, ParamDirection.INOUT)]

        has_request_body = bool(in_params)

        lines.append(f"{inner_indent}DasResult serial_result = DAS_S_OK;")
        lines.append(f"{inner_indent}(void)serial_result;")

        if has_request_body:
            lines.append(f"{inner_indent}MemorySerializerReader reader(params, params_size);")
            lines.append("")

            # 反序列化参数（V3 Body Header 已在 IStubBase::HandleMessage 中解析）
            lines.append(f"{inner_indent}// Parameters (V3 Body after 16-byte header)")
            for param in in_params:
                deserialize_code = self._generate_deserialize_param_for_stub(param, inner_indent)
                for line in deserialize_code:
                    lines.append(f"{line}")
            lines.append("")

        # Handle 方法参数名列表（用于检测变量名冲突）
        handle_param_names = {'impl', 'params', 'params_size', 'out_response', 'object_manager'}

        # 构建完整类型名（带指针）用于接口检测
        def _get_full_type_name(type_info):
            base = type_info.base_type
            if type_info.is_pointer:
                base += "*" * type_info.pointer_level
            return base

        for param in out_params:
            # 处理变量名冲突：INOUT 参数可能与 Handle 方法参数名冲突
            local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name

            # For [out] interface params: IDL gives IDasBinaryBuffer*, ABI expects IDasBinaryBuffer**
            # Declare with IDL-level pointer count (one less than ABI), pass &var to impl
            full_type_name = _get_full_type_name(param.type_info)
            if param.direction in (ParamDirection.OUT, ParamDirection.INOUT) and self.type_mapper.is_interface_type(full_type_name):
                # Declare with one less pointer level (IDL level)
                base_cpp = self._get_cpp_type_base(param.type_info)  # IDasBinaryBuffer*
                lines.append(f"{inner_indent}{base_cpp} {local_name} = {{}};")
            else:
                cpp_type = self._get_cpp_type(param.type_info)
                lines.append(f"{inner_indent}{cpp_type} {local_name} = {{}};")

        lines.append(f"{inner_indent}MemorySerializerWriter writer;")
        lines.append("")

        call_params = []
        for param in method.parameters:
            # 处理变量名冲突
            local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name

            if param.direction in (ParamDirection.OUT, ParamDirection.INOUT):
                # For interface [out] params: ABI expects extra * so always pass &var
                full_type_name = _get_full_type_name(param.type_info)
                if self.type_mapper.is_interface_type(full_type_name):
                    call_params.append(f"&{local_name}")
                elif param.type_info.is_pointer and param.type_info.pointer_level >= 1:
                    call_params.append(f"{local_name}")
                else:
                    call_params.append(f"&{local_name}")
            else:
                call_params.append(f"{local_name}")

        params_str = ", ".join(call_params) if call_params else ""

        # impl is already validated in DispatchMethod
        if has_return:
            cpp_return_type = self._get_cpp_type(method.return_type)
            lines.append(f"{inner_indent}{cpp_return_type} call_result = impl->{method.name}({params_str});")
        else:
            lines.append(f"{inner_indent}impl->{method.name}({params_str});")
        lines.append("")

        lines.append(f"{inner_indent}serial_result = writer.WriteInt32(DAS_S_OK);")
        lines.append(f"{inner_indent}if (DAS::IsFailed(serial_result))")
        lines.append(f"{inner_indent}{{")
        lines.append(f"{inner_indent}    return serial_result;")
        lines.append(f"{inner_indent}}}")
        lines.append("")

        if has_return:
            serialize_return_code = self._generate_serialize_return_for_stub(method.return_type, inner_indent)
            for line in serialize_return_code:
                lines.append(f"{line}")

        for param in out_params:
            # 处理变量名冲突
            local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name
            serialize_code = self._generate_serialize_param_for_stub(param, inner_indent, local_name)
            for line in serialize_code:
                lines.append(f"{line}")

        lines.append(f"{inner_indent}out_response = writer.GetBuffer();")
        lines.append("")
        lines.append(f"{inner_indent}return DAS_S_OK;")
        lines.append(f"}}")

        return "\n".join(lines)
    
    def _generate_deserialize_param_for_stub(self, param: ParameterDef, indent: str) -> List[str]:
        """生成参数反序列化代码（用于 Stub 从请求体读取参数）"""
        lines = []

        # Handle 方法参数名列表（用于检测变量名冲突）
        handle_param_names = {'impl', 'params', 'params_size', 'out_response', 'object_manager'}
        local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name

        # 检查是否是接口指针类型
        if self.type_mapper.is_interface_type(param.type_info.base_type):
            interface_name = self.type_mapper.get_interface_name(param.type_info.base_type)
            proxy_name = f"{interface_name}Proxy"
            param_name = local_name

            # 反序列化接口指针：先读 ObjectId，再判断本地/远程
            lines.append(f"{indent}// 反序列化接口指针: {interface_name}*")
            lines.append(f"{indent}uint64_t {param_name}_encoded = reader.ReadUInt64();")
            lines.append(f"{indent}ObjectId {param_name}_id = DecodeObjectId({param_name}_encoded);")
            lines.append(f"{indent}if (object_manager.IsLocalObject({param_name}_id))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    // 本地对象：直接查找")
            lines.append(f"{indent}    void* {param_name}_ptr = nullptr;")
            lines.append(f"{indent}    serial_result = object_manager.LookupObject({param_name}_encoded, &{param_name}_ptr);")
            lines.append(f"{indent}    if (DAS::IsFailed(serial_result))")
            lines.append(f"{indent}    {{")
            lines.append(f"{indent}        return serial_result;")
            lines.append(f"{indent}    }}")
            lines.append(f"{indent}    {param_name} = static_cast<{interface_name}*>({param_name}_ptr);")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}else")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    // 远程对象：创建 Proxy")
            lines.append(f"{indent}    // 注意：需要通过 IpcRunLoop 获取 business_thread")
            lines.append(f"{indent}    {param_name} = nullptr;  // TODO: 使用 CreateProxyByInterfaceId 创建远程代理")
            lines.append(f"{indent}}}")
            return lines

        type_info = self.type_mapper.get_type_info(param.type_info.base_type)

        if type_info is None:
            cpp_type = self._get_cpp_type(param.type_info)
            lines.append(f"{indent}{cpp_type} {local_name} = {{}};")
            lines.append(f"{indent}// TODO: Deserialize type {param.type_info.base_type}")
            return lines

        cpp_type, _, read_method, is_struct = type_info

        if is_struct:
            # 使用内联字段展开进行反序列化
            struct_def = self.type_mapper.struct_defs.get(param.type_info.base_type)
            if struct_def and struct_def.fields:
                lines.append(f"{indent}{param.type_info.base_type} {local_name};")
                for field in struct_def.fields:
                    field_cpp_type, _, field_read_method, field_is_struct = self.type_mapper.get_type_info(field.type_name) or (None, None, None, False)
                    if field_is_struct:
                        # 嵌套 struct：递归内联展开
                        lines.append(f"{indent}// Nested struct field: {field.name}")
                        nested_struct_def = self.type_mapper.struct_defs.get(field.type_name)
                        if nested_struct_def and nested_struct_def.fields:
                            for nested_field in nested_struct_def.fields:
                                nested_field_type_info = self.type_mapper.get_type_info(nested_field.type_name)
                                if nested_field_type_info:
                                    nf_cpp_type, _, nf_read_method, _ = nested_field_type_info
                                    lines.append(f"{indent}serial_result = reader.{nf_read_method}(&{local_name}.{field.name}.{nested_field.name});")
                                    lines.append(f"{indent}if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
                    elif field_cpp_type and field_read_method:
                        lines.append(f"{indent}serial_result = reader.{field_read_method}(&{local_name}.{field.name});")
                        lines.append(f"{indent}if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
            else:
                lines.append(f"{indent}{param.type_info.base_type} {local_name};")
                lines.append(f"{indent}// TODO: Deserialize struct {param.type_info.base_type}")
        else:
            lines.append(f"{indent}{cpp_type} {local_name};")
            lines.append(f"{indent}serial_result = reader.{read_method}(&{local_name});")

        lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return serial_result;")
        lines.append(f"{indent}}}")

        return lines
    
    def _generate_serialize_param_for_stub(self, param: ParameterDef, indent: str, local_name: str = None) -> List[str]:
        """生成参数序列化代码（用于 Stub 向响应体写入输出参数）"""
        lines = []
        type_info = self.type_mapper.get_type_info(param.type_info.base_type)

        # 使用 local_name 如果提供，否则使用 param.name
        var_name = local_name if local_name else param.name

        if type_info is None:
            lines.append(f"{indent}// TODO: Serialize type {param.type_info.base_type}")
            return lines

        cpp_type, write_method, _, is_struct = type_info

        if is_struct:
            # 使用内联字段展开进行序列化
            struct_def = self.type_mapper.struct_defs.get(param.type_info.base_type)
            if struct_def and struct_def.fields:
                # 判断参数是否为指针类型，决定使用 -> 还是 .
                accessor = "->" if param.type_info.is_pointer else "."
                for field in struct_def.fields:
                    field_cpp_type, field_write_method, _, field_is_struct = self.type_mapper.get_type_info(field.type_name) or (None, None, None, False)
                    if field_is_struct:
                        # 嵌套 struct：递归内联展开
                        nested_struct_def = self.type_mapper.struct_defs.get(field.type_name)
                        if nested_struct_def and nested_struct_def.fields:
                            for nested_field in nested_struct_def.fields:
                                nested_field_type_info = self.type_mapper.get_type_info(nested_field.type_name)
                                if nested_field_type_info:
                                    _, nf_write_method, _, _ = nested_field_type_info
                                    lines.append(f"{indent}serial_result = writer.{nf_write_method}({var_name}{accessor}{field.name}.{nested_field.name});")
                                    lines.append(f"{indent}if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
                    elif field_cpp_type and field_write_method:
                        lines.append(f"{indent}serial_result = writer.{field_write_method}({var_name}{accessor}{field.name});")
                        lines.append(f"{indent}if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
            else:
                lines.append(f"{indent}// TODO: Serialize struct {param.type_info.base_type}")
        else:
            if param.type_info.is_pointer and param.type_info.pointer_level >= 1:
                lines.append(f"{indent}serial_result = writer.{write_method}(*{var_name});")
            else:
                lines.append(f"{indent}serial_result = writer.{write_method}({var_name});")
        
        lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return serial_result;")
        lines.append(f"{indent}}}")
        
        return lines
    
    def _generate_serialize_return_for_stub(self, return_type: TypeInfo, indent: str) -> List[str]:
        """生成返回值序列化代码（用于 Stub 向响应体写入返回值）"""
        lines = []
        type_info = self.type_mapper.get_type_info(return_type.base_type)
        
        if type_info is None:
            lines.append(f"{indent}// TODO: Serialize return value of type {return_type.base_type}")
            return lines
        
        cpp_type, write_method, _, is_struct = type_info

        if is_struct:
            # 使用内联字段展开进行序列化
            struct_def = self.type_mapper.struct_defs.get(return_type.base_type)
            if struct_def and struct_def.fields:
                for field in struct_def.fields:
                    field_cpp_type, field_write_method, _, field_is_struct = self.type_mapper.get_type_info(field.type_name) or (None, None, None, False)
                    if field_is_struct:
                        # 嵌套 struct：递归内联展开
                        nested_struct_def = self.type_mapper.struct_defs.get(field.type_name)
                        if nested_struct_def and nested_struct_def.fields:
                            for nested_field in nested_struct_def.fields:
                                nested_field_type_info = self.type_mapper.get_type_info(nested_field.type_name)
                                if nested_field_type_info:
                                    _, nf_write_method, _, _ = nested_field_type_info
                                    lines.append(f"{indent}serial_result = writer.{nf_write_method}(call_result.{field.name}.{nested_field.name});")
                                    lines.append(f"{indent}if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
                    elif field_cpp_type and field_write_method:
                        lines.append(f"{indent}serial_result = writer.{field_write_method}(call_result.{field.name});")
                        lines.append(f"{indent}if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
            else:
                lines.append(f"{indent}// TODO: Serialize struct {return_type.base_type}")
        else:
            lines.append(f"{indent}serial_result = writer.{write_method}(call_result);")
        
        lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return serial_result;")
        lines.append(f"{indent}}}")
        
        return lines
    
    def _generate_stub_class(self, interface: InterfaceDef, namespace_depth: int = 0) -> str:
        """为接口生成 Stub 类（继承 IStubBase）"""
        lines = []
        indent = "    " * namespace_depth
        class_indent = "    " * (namespace_depth + 1)
        method_indent = "    " * (namespace_depth + 2)

        interface_short_name = self._get_interface_short_name(interface.name)
        class_name = f"{interface_short_name}Stub"
        interface_id = fnv1a_hash_guid(interface.uuid)

        # 收集继承链全部方法
        all_methods = self._collect_all_methods(interface)

        lines.append(f"{indent}// ============================================================================")
        lines.append(f"{indent}// {class_name}")
        lines.append(f"{indent}// IPC Stub for {interface.name}")
        lines.append(f"{indent}// Interface UUID: {interface.uuid}")
        lines.append(f"{indent}// ============================================================================")
        lines.append("")

        lines.append(f"{indent}class {class_name} : public IStubBase")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}public:")

        lines.append(f"{class_indent}static constexpr uint32_t InterfaceId = 0x{interface_id:08X}u;")
        lines.append("")

        lines.append(self._generate_method_constants(all_methods, namespace_depth))
        lines.append("")

        # GetInterfaceId override
        lines.append(f"{class_indent}[[nodiscard]] uint32_t GetInterfaceId() const noexcept override")
        lines.append(f"{class_indent}{{")
        lines.append(f"{class_indent}    return InterfaceId;")
        lines.append(f"{class_indent}}}")
        lines.append("")

        # DispatchMethod override
        lines.append(self._generate_dispatch_method(interface, all_methods, namespace_depth))
        lines.append("")

        lines.append(f"{indent}private:")

        # Generate Handle method declarations (instance methods, not static)
        for method, _ in all_methods:
            lines.append(self._generate_handle_method_declaration(interface, method, namespace_depth))

        lines.append(f"{indent}}};")
        lines.append("")

        return "\n".join(lines)
    
    def _generate_handle_method_definitions(self, interface: InterfaceDef, all_methods: list, namespace_depth: int = 0) -> str:
        """生成所有 Handle 方法的定义"""
        lines = []
        indent = "    " * namespace_depth
        for i, (method, _) in enumerate(all_methods):
            definition = self._generate_handle_method_definition(interface, method, i)
            for line in definition.split('\n'):
                lines.append(f"{indent}{line}")
            lines.append("")
        return "\n".join(lines)
    
    def _generate_interface_json(self, interface: InterfaceDef) -> Dict[str, Any]:
        """生成接口元数据 JSON"""
        interface_id = fnv1a_hash_guid(interface.uuid)
        all_methods = self._collect_all_methods(interface)
        methods = []
        for i, (method, _) in enumerate(all_methods):
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
    
    def generate_stub_headers(self, output_dir: str, cache_dir: Optional[str] = None, abi_dir: Optional[str] = None) -> List[str]:
        """生成所有接口的 IPC Stub 头文件

        Args:
            output_dir: 输出目录
            cache_dir: 缓存目录（用于写入 interface.json）
            abi_dir: ABI 头文件目录（用于收集接口类型 includes）

        Returns:
            生成的文件路径列表
        """
        generated_files = []

        stub_dir = os.path.join(output_dir, "stub")
        os.makedirs(stub_dir, exist_ok=True)

        if cache_dir:
            os.makedirs(cache_dir, exist_ok=True)

        # 从 ABI 目录加载接口命名空间和头文件映射
        # 如果未指定 abi_dir，自动从 output_dir 的父目录推导
        if not abi_dir:
            abi_dir = os.path.join(os.path.dirname(output_dir), "abi")
        self.type_mapper.load_namespaces_from_abi_dir(abi_dir)

        for interface in self.document.interfaces:
            interface_short_name = self._get_interface_short_name(interface.name)
            filename = f"{interface_short_name}Stub.h"
            filepath = os.path.join(stub_dir, filename)

            ns_prefix = ""
            if interface.namespace:
                ns_prefix = interface.namespace.replace("::", "_") + "_"
            guard_name = f"DAS_IPC_{ns_prefix}{interface_short_name.upper()}_STUB_H"

            # 获取 ABI 头文件名称
            abi_header = self.type_mapper.interface_header_files.get(interface.name, f"{interface.name}.h")

            # 收集接口类型 includes
            interface_includes = self._collect_interface_includes(interface)

            content = self._file_header(guard_name, interface.name, abi_header, interface_includes)

            ns_depth = 0
            if interface.namespace:
                content += self._generate_namespace_open(interface.namespace)
                ns_depth = len(interface.namespace.split("::"))

            # IPC 和 Stub 命名空间缩进
            ipc_ns_indent = "    " * ns_depth
            stub_ns_indent = "    " * (ns_depth + 1)

            # 对于没有命名空间的接口，使用不同的命名空间名称以避免冲突
            if interface.namespace:
                content += f"{ipc_ns_indent}namespace IPC\n"
            else:
                content += f"{ipc_ns_indent}namespace DasIpc\n"
            content += f"{ipc_ns_indent}{{\n"
            content += f"{stub_ns_indent}namespace Stub\n"
            content += f"{stub_ns_indent}{{\n"

            # 添加 using 声明以解析其他命名空间中的接口类型
            content += f"{stub_ns_indent}using namespace Das::ExportInterface;\n"
            content += f"{stub_ns_indent}using namespace Das::PluginInterface;\n"

            ns_depth += 2

            content += self._generate_stub_class(interface, ns_depth)
            all_methods = self._collect_all_methods(interface)
            content += self._generate_handle_method_definitions(interface, all_methods, ns_depth)

            # 关闭命名空间
            stub_ns_indent = "    " * (ns_depth - 1)
            ipc_ns_indent = "    " * (ns_depth - 2)
            content += f"{stub_ns_indent}}}\n"
            content += f"{stub_ns_indent}// namespace Stub\n"
            content += f"{ipc_ns_indent}}}\n"
            # 使用与打开时相同的命名空间名称
            ipc_ns_name = "IPC" if interface.namespace else "DasIpc"
            content += f"{ipc_ns_indent}// namespace {ipc_ns_name}\n"

            if interface.namespace:
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

    def _generate_stub_factory(self) -> str:
        """生成 StubFactory 结构"""
        lines = []
        lines.append("")
        lines.append("// StubFactory 结构（由 IDL 生成器生成）")
        lines.append("namespace DasIpcStub {")
        lines.append("")

        # 生成 StubFactory 结构体
        lines.append("struct StubFactory {")
        lines.append("    void RegisterAll(IpcRunLoop& run_loop) {")

        for interface in self.document.interfaces:
            interface_short_name = self._get_interface_short_name(interface.name)
            stub_name = f"{interface_short_name}Stub"
            lines.append(f"        run_loop.RegisterHandler(HeaderFlags::NONE, {interface.name}::InterfaceId, &{stub_name}_);")

        lines.append("    }")
        lines.append("")
        lines.append("    // Stub 实例")
        for interface in self.document.interfaces:
            interface_short_name = self._get_interface_short_name(interface.name)
            stub_name = f"{interface_short_name}Stub"
            lines.append(f"    [[no_unique_address]] {stub_name} {stub_name}_;")

        lines.append("};")
        lines.append("")
        lines.append("inline StubFactory g_stub_factory;")
        lines.append("")
        lines.append("} // namespace DasIpcStub")

        return "\n".join(lines)

    def generate_stub_factory(self, output_dir: str) -> str:
        """生成 StubFactory 到单独文件"""
        from datetime import datetime, timezone
        timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

        factory_content = f"""// This file is automatically generated by DAS IDL Generator
// Generated at: {timestamp}
// !!! DO NOT EDIT !!!

#ifndef DAS_IPC_STUB_FACTORY_H
#define DAS_IPC_STUB_FACTORY_H

#include <das/Core/IPC/IStubBase.h>
#include <das/Core/IPC/IpcRunLoop.h>

"""

        factory_content += self._generate_stub_factory()
        factory_content += """

#endif // DAS_IPC_STUB_FACTORY_H
"""
        filepath = os.path.join(output_dir, "IpcStubFactory.h")
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(factory_content)
        print(f"Generated: {filepath}")
        return filepath


def generate_ipc_stub_files(
    document: IdlDocument,
    output_dir: str,
    base_name: Optional[str] = None,
    idl_file_path: Optional[str] = None,
    cache_dir: Optional[str] = None,
    abi_dir: Optional[str] = None
) -> List[str]:
    """生成 IPC Stub 文件

    Args:
        document: IDL 文档对象
        output_dir: 输出目录
        base_name: 基础文件名（可选）
        idl_file_path: IDL 文件路径（可选）
        cache_dir: 缓存目录（用于写入 interface.json）
        abi_dir: ABI 头文件目录（用于收集接口类型 includes）

    Returns:
        生成的文件路径列表
    """
    generator = IpcStubGenerator(document, idl_file_path)
    return generator.generate_stub_headers(output_dir, cache_dir, abi_dir)


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
    generator = IpcStubGenerator(doc, "test.idl")
    
    print("=== 生成的 IPC Stub 头文件 ===")
    files = generator.generate_stub_headers("./test_output")
    for f in files:
        print(f"  - {f}")
