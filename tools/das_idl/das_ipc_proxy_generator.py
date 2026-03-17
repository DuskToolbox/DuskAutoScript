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
ParamDirection = _das_idl_parser.ParamDirection
StructDef = _das_idl_parser.StructDef


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


def fnv1a_hash_guid(guid_str: str) -> int:
    """计算 GUID 字符串的 FNV-1a 32-bit hash（用于生成 interface_id）
    
    处理两种格式:
    - {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
    - xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    
    大小写不敏感
    
    Args:
        guid_str: GUID 字符串
        
    Returns:
        32-bit FNV-1a hash 值
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


class IpcProxyGenerator:
    """IPC Proxy 代码生成器"""

    def __init__(self, document: IdlDocument, idl_file_path: Optional[str] = None,
                 extra_interface_namespaces: Optional[Dict[str, str]] = None):
        self.document = document
        self.idl_file_path = idl_file_path
        self.idl_file_name = os.path.basename(idl_file_path) if idl_file_path else None
        self.indent = "    "  # 4 空格缩进
        self.type_mapper = ProxyTypeMapper(document)
        # 额外的接口命名空间映射（来自其他 IDL 文件的接口）
        if extra_interface_namespaces:
            self.type_mapper.interface_namespaces.update(extra_interface_namespaces)
    
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
#include <das/Core/IPC/MemorySerializer.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/Serializer.h>
#include <das/Core/Logger/Logger.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "{abi_header_name}"

"""

        # 添加方法参数中使用的接口类型的头文件
        if interface:
            interface_includes = self._collect_interface_includes(interface)
            if interface_includes:
                includes_str = "\n".join(
                    f'#include "{inc}"' for inc in interface_includes
                )
                result += f"{includes_str}\n"

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
        if interface.methods:
            lines.append(f"{class_indent}static constexpr MethodMetadata MethodTable[] = {{")
            for i, method in enumerate(interface.methods):
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
        lines.append(f"{inner_indent}if (GetObjectManager())")
        lines.append(f"{inner_indent}{{")
        lines.append(f"{inner_indent}    GetObjectManager()->Release(GetObjectId());")
        lines.append(f"{inner_indent}}}")
        lines.append(f"{inner_indent}delete this;")
        lines.append(f"{method_indent}}}")
        lines.append(f"{method_indent}return count;")
        lines.append(f"{class_indent}}}")
        lines.append("")
        
        for i, method in enumerate(interface.methods):
            method_sig = self._generate_method_signature(interface, method, interface.namespace)
            lines.append(f"{class_indent}{method_sig}")
            lines.append(f"{class_indent}{{")
            method_body = self._generate_method_body_v2(interface, method, i, namespace_depth)
            for line in method_body.splitlines():
                lines.append(f"{line}")
            lines.append(f"{class_indent}}}")
            lines.append("")
        
        lines.append(f"{indent}private:")
        lines.append(f"{class_indent}std::atomic<uint32_t> ref_count_{{1}};")
        lines.append(f"{class_indent}{interface.name}* local_impl_ = nullptr;")
        lines.append(f"{indent}}};")
        lines.append("")
        
        return "\n".join(lines)
    
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
        lines.append(f"{indent}auto* obj_mgr = GetObjectManager();")
        lines.append(f"{indent}if (obj_mgr->IsLocalObject(GetObjectId())) {{")
        lines.append(f"{indent}    void* obj_ptr = nullptr;")
        lines.append(f"{indent}    obj_mgr->LookupObject(GetObjectId(), &obj_ptr);")
        lines.append(f"{indent}    auto* local_impl = static_cast<{interface.name}*>(obj_ptr);")

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

        if need_request_body:
            lines.append(f"{indent}MemorySerializerWriter writer;")
            lines.append(f"{indent}DasResult ipc_result = DAS_S_OK;")
            lines.append(f"{indent}(void)ipc_result;")

            # V3: 写入 interface_id (4 bytes)
            lines.append(f"{indent}// V3 Body Header: interface_id")
            lines.append(f"{indent}ipc_result = writer.WriteUInt32(InterfaceId);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result)) {{ return ipc_result; }}")

            # V3: 写入 method_id (2 bytes)
            lines.append(f"{indent}// V3 Body Header: method_id")
            lines.append(f"{indent}ipc_result = writer.WriteUInt16({method_index});")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result)) {{ return ipc_result; }}")

            # V3: 写入 reserved (2 bytes)
            lines.append(f"{indent}// V3 Body Header: reserved (alignment)")
            lines.append(f"{indent}ipc_result = writer.WriteUInt16(0);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result)) {{ return ipc_result; }}")

            # V3: 写入 ObjectId (8 bytes)
            lines.append(f"{indent}// V3 Body Header: target_object ObjectId")
            lines.append(f"{indent}ipc_result = writer.WriteUInt16(GetObjectId().session_id);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result)) {{ return ipc_result; }}")
            lines.append(f"{indent}ipc_result = writer.WriteUInt16(GetObjectId().generation);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result)) {{ return ipc_result; }}")
            lines.append(f"{indent}ipc_result = writer.WriteUInt32(GetObjectId().local_id);")
            lines.append(f"{indent}if (DAS::IsFailed(ipc_result)) {{ return ipc_result; }}")

            # 序列化参数
            lines.append(f"{indent}// V3 Body: parameters")
            for param in in_params:
                serialize_code = self._generate_serialize_param(param, indent)
                for line in serialize_code:
                    lines.append(f"{line}")
            lines.append("")
            lines.append(f"{indent}const std::vector<uint8_t>& request_body = writer.GetBuffer();")
        else:
            lines.append(f"{indent}const uint8_t* request_body = nullptr;")
            lines.append(f"{indent}size_t request_body_size = 0;")
        
        lines.append("")
        lines.append(f"{indent}std::vector<uint8_t> response_body;")
        if need_request_body:
            lines.append(f"{indent}ipc_result = SendRequest({method_index},")
            lines.append(f"{indent}    request_body.data(), request_body.size(), response_body);")
        else:
            lines.append(f"{indent}DasResult ipc_result = SendRequest({method_index},")
            lines.append(f"{indent}    request_body, request_body_size, response_body);")
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
                deserialize_code = self._generate_deserialize_param(param, indent, has_return)
                for line in deserialize_code:
                    lines.append(f"{line}")
            
            if has_return:
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

        # 检查是否是接口指针类型
        if self.type_mapper.is_interface_type(param.type_info.base_type):
            interface_name = self.type_mapper.get_interface_name(param.type_info.base_type)
            param_name = param.name

            # 获取 ObjectId 并序列化
            lines.append(f"{indent}// 序列化接口指针: {interface_name}*")
            lines.append(f"{indent}ObjectId {param_name}_id = GetObjectIdFromInterface({param_name});")
            lines.append(f"{indent}ipc_result = writer.WriteUInt64(EncodeObjectId({param_name}_id));")
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
    
    def _generate_deserialize_param(self, param: ParameterDef, indent: str, has_return: bool = True) -> List[str]:
        """生成参数反序列化代码（用于 [out] 和 [inout] 参数）"""
        lines = []
        type_info = self.type_mapper.get_type_info(param.type_info.base_type)
        
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
            lines.append(f"{class_indent}{method_sig}")
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
        lines.append(f"{class_indent}{interface.name}* local_impl_;")
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
