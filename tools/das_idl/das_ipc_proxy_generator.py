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
        for struct in document.structs:
            self.struct_types.add(struct.name)
            # 也支持带命名空间的 struct
            if struct.namespace:
                full_name = f"{struct.namespace}::{struct.name}"
                self.struct_types.add(full_name)
    
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
    
    def __init__(self, document: IdlDocument, idl_file_path: Optional[str] = None):
        self.document = document
        self.idl_file_path = idl_file_path
        self.idl_file_name = os.path.basename(idl_file_path) if idl_file_path else None
        self.indent = "    "  # 4 空格缩进
        self.type_mapper = ProxyTypeMapper(document)
    
    def _file_header(self, guard_name: str, interface_name: str) -> str:
        """生成文件头"""
        timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
        
        idl_file_comment = ""
        if self.idl_file_name:
            idl_file_comment = f"// Source IDL file: {self.idl_file_name}\n"
        
        return f"""#if !defined({guard_name})
#define {guard_name}

// This file is automatically generated by DAS IPC Proxy Generator
// Generated at: {timestamp}
{idl_file_comment}// !!! DO NOT EDIT !!!
//
// IPC Proxy for {interface_name}
//

#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/MemorySerializer.h>
#include <das/Core/IPC/Serializer.h>
#include <cstdint>
#include <string>
#include <vector>

"""
    
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
    
    def _get_interface_short_name(self, interface_name: str) -> str:
        """从接口名获取短名称（去掉 I 前缀）
        
        IDasLogger -> DasLogger
        """
        if interface_name.startswith('IDas'):
            return interface_name[1:]
        if interface_name.startswith('I') and len(interface_name) > 1:
            return interface_name[1:]
        return interface_name
    
    def _generate_method_signature(self, interface: InterfaceDef, method: MethodDef) -> str:
        """生成方法签名"""
        return_type = self._get_cpp_type(method.return_type)
        
        params = []
        for param in method.parameters:
            param_type = self._get_cpp_type(param.type_info)
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
        lines.append(f"{inner_indent}DasResult result = transport_.SendAndWait(")
        lines.append(f"{inner_indent}    &request, sizeof(request),")
        lines.append(f"{inner_indent}    &response, sizeof(response));")
        lines.append("")
        
        # Step 4: 处理响应
        lines.append(f"{inner_indent}// Check result")
        lines.append(f"{inner_indent}if (DAS_FAILED(result))")
        lines.append(f"{inner_indent}{{")
        lines.append(f"{inner_indent}    return result;")
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
        """为接口生成 Proxy 类，继承 IPCProxyBase"""
        lines = []
        indent = "    " * namespace_depth
        class_indent = "    " * (namespace_depth + 1)
        method_indent = "    " * (namespace_depth + 2)
        
        interface_short_name = self._get_interface_short_name(interface.name)
        class_name = f"{interface_short_name}Proxy"
        interface_id = fnv1a_hash_guid(interface.uuid)
        
        lines.append(f"{indent}class {class_name} : public DasProxyBase<{interface.name}>")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}public:")
        lines.append(f"{class_indent}static constexpr uint32_t InterfaceId = 0x{interface_id:08X}u;")
        lines.append("")
        lines.append(f"{class_indent}static constexpr MethodMetadata MethodTable[] = {{")
        for i, method in enumerate(interface.methods):
            method_hash = fnv1a_hash(f"{interface.name}::{method.name}")
            lines.append(f"{method_indent}{{ {i}, \"{method.name}\", 0x{method_hash:08X}u }},")
        lines.append(f"{class_indent}}};")
        lines.append("")
        lines.append(f"{class_indent}{class_name}(")
        lines.append(f"{class_indent}    const ObjectId& object_id,")
        lines.append(f"{class_indent}    IpcRunLoop* run_loop,")
        lines.append(f"{class_indent}    DistributedObjectManager* object_manager)")
        lines.append(f"{class_indent}    : DasProxyBase<{interface.name}>(InterfaceId, object_id, run_loop, object_manager)")
        lines.append(f"{class_indent}{{")
        lines.append(f"{class_indent}}}")
        lines.append("")
        
        for i, method in enumerate(interface.methods):
            method_sig = self._generate_method_signature(interface, method)
            lines.append(f"{class_indent}{method_sig}")
            lines.append(f"{class_indent}{{")
            method_body = self._generate_method_body_v2(interface, method, i, namespace_depth)
            for line in method_body.splitlines():
                lines.append(f"{line}")
            lines.append(f"{class_indent}}}")
            lines.append("")
        
        lines.append(f"{indent}private:")
        lines.append(f"{class_indent}{interface.name}* local_impl_ = nullptr;")
        lines.append(f"{indent}}};")
        lines.append("")
        
        return "\n".join(lines)
    
    def _generate_method_body_v2(self, interface: InterfaceDef, method: MethodDef, method_index: int, namespace_depth: int = 0) -> str:
        """生成方法体，使用基类的 SendRequest 方法
        
        Generated Code Pattern:
        1. 序列化输入参数
        2. 调用 SendRequest
        3. 反序列化远程返回码
        4. 如果远程返回码失败则返回
        5. 反序列化输出参数
        """
        indent = "    " * (namespace_depth + 2)
        inner_indent = "    " * (namespace_depth + 3)
        lines = []
        
        return_type = method.return_type.base_type
        has_return = return_type != 'void'
        
        in_params = [p for p in method.parameters if p.direction != ParamDirection.OUT]
        out_params = [p for p in method.parameters if p.direction in (ParamDirection.OUT, ParamDirection.INOUT)]
        
        need_request_body = bool(in_params)
        need_response_body = bool(out_params) or has_return
        
        if need_request_body:
            lines.append(f"{indent}MemorySerializerWriter writer;")
            lines.append(f"{indent}DasResult result = DAS_S_OK;")
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
            lines.append(f"{indent}result = SendRequest({method_index},")
            lines.append(f"{indent}    request_body.data(), request_body.size(), response_body);")
        else:
            lines.append(f"{indent}DasResult result = SendRequest({method_index},")
            lines.append(f"{indent}    request_body, request_body_size, response_body);")
        lines.append(f"{indent}if (DAS_FAILED(result))")
        lines.append(f"{indent}{{")
        if has_return:
            lines.append(f"{indent}    return result;")
        else:
            lines.append(f"{indent}    return;")
        lines.append(f"{indent}}}")
        lines.append("")
        
        if need_response_body:
            lines.append(f"{indent}MemorySerializerReader reader(response_body);")
            lines.append("")
            lines.append(f"{indent}DasResult remote_result;")
            lines.append(f"{indent}result = reader.ReadInt32(&remote_result);")
            lines.append(f"{indent}if (DAS_FAILED(result))")
            lines.append(f"{indent}{{")
            if has_return:
                lines.append(f"{indent}    return result;")
            else:
                lines.append(f"{indent}    return;")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}if (DAS_FAILED(remote_result))")
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
        type_info = self.type_mapper.get_type_info(param.type_info.base_type)
        
        if type_info is None:
            lines.append(f"{indent}// TODO: Unknown type {param.type_info.base_type}")
            lines.append(f"{indent}// result = writer.WriteCustom<{param.type_info.base_type}>({param.name});")
            return lines
        
        cpp_type, write_method, _, is_struct = type_info
        
        if is_struct:
            lines.append(f"{indent}result = Serialize_{param.type_info.base_type}(writer, {param.name});")
        else:
            # 处理指针和引用
            if param.type_info.is_pointer or param.type_info.is_reference:
                if param.type_info.pointer_level == 1:
                    lines.append(f"{indent}result = writer.{write_method}(*{param.name});")
                else:
                    lines.append(f"{indent}result = writer.{write_method}({param.name});")
            else:
                lines.append(f"{indent}result = writer.{write_method}({param.name});")
        
        lines.append(f"{indent}if (DAS_FAILED(result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return result;")
        lines.append(f"{indent}}}")
        
        return lines
    
    def _generate_deserialize_param(self, param: ParameterDef, indent: str, has_return: bool = True) -> List[str]:
        """生成参数反序列化代码（用于 [out] 和 [inout] 参数）"""
        lines = []
        type_info = self.type_mapper.get_type_info(param.type_info.base_type)
        
        if type_info is None:
            lines.append(f"{indent}// TODO: Unknown type {param.type_info.base_type}")
            lines.append(f"{indent}// result = reader.ReadCustom<{param.type_info.base_type}>({param.name});")
            return lines
        
        cpp_type, _, read_method, is_struct = type_info
        
        if is_struct:
            lines.append(f"{indent}result = Deserialize_{param.type_info.base_type}(reader, {param.name});")
        else:
            if param.type_info.is_pointer and param.type_info.pointer_level >= 1:
                lines.append(f"{indent}result = reader.{read_method}({param.name});")
            else:
                lines.append(f"{indent}result = reader.{read_method}(&{param.name});")
        
        lines.append(f"{indent}if (DAS_FAILED(result))")
        lines.append(f"{indent}{{")
        if has_return:
            lines.append(f"{indent}    return result;")
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
            lines.append(f"{indent}result = Deserialize_{return_type.base_type}(reader, &ret_value);")
            lines.append(f"{indent}if (DAS_FAILED(result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return result;")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}return ret_value;")
        else:
            lines.append(f"{indent}{cpp_type} ret_value;")
            lines.append(f"{indent}result = reader.{read_method}(&ret_value);")
            lines.append(f"{indent}if (DAS_FAILED(result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return result;")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}return ret_value;")
        
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
        lines.append(f"{class_indent}static constexpr MethodMetadata MethodTable[] = {{")
        for i, method in enumerate(interface.methods):
            method_hash = fnv1a_hash(f"{interface.name}::{method.name}")
            lines.append(f"{method_indent}{{ /* method_id */ {i}, /* name */ \"{method.name}\", /* hash */ 0x{method_hash:08X}u }},")
        lines.append(f"{class_indent}}};")
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
            method_sig = self._generate_method_signature(interface, method)
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
        
        for interface in self.document.interfaces:
            interface_short_name = self._get_interface_short_name(interface.name)
            filename = f"{interface_short_name}Proxy.h"
            filepath = os.path.join(proxy_dir, filename)
            
            ns_prefix = ""
            if interface.namespace:
                ns_prefix = interface.namespace.replace("::", "_") + "_"
            guard_name = f"DAS_IPC_{ns_prefix}{interface_short_name.upper()}_PROXY_H"
            
            content = self._file_header(guard_name, interface.name)
            
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
                ns_depth += 2
            
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
