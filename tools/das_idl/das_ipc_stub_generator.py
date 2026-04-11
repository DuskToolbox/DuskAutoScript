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

from ipc_common import fnv1a_hash_guid, fnv1a_hash, BUILTIN_INTERFACE_HASHES, properties_to_methods, resolve_import_chain
from ipc_type_mapper import StubTypeMapper

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
PropertyDef = _das_idl_parser.PropertyDef
parse_idl_file = _das_idl_parser.parse_idl_file


class IpcStubGenerator:
    """IPC Stub 代码生成器"""
    
    def __init__(self, document: IdlDocument, idl_file_path: Optional[str] = None):
        self.document = document
        self.idl_file_path = idl_file_path
        self.idl_file_name = os.path.basename(idl_file_path) if idl_file_path else None
        self.indent = "    "  # 4 空格缩进
        self.type_mapper = StubTypeMapper(document)
        # Fixed sizes for [out] parameter serialization
        # Interface pointers [out] = 12 bytes (session_id 2B + generation 2B + local_id 4B + interface_id 4B)
        self.FIXED_SIZES = {
            'int8_t': 1, 'uint8_t': 1, 'int16_t': 2, 'uint16_t': 2,
            'int32_t': 4, 'uint32_t': 4, 'int64_t': 8, 'uint64_t': 8,
            'float': 4, 'double': 8,
            'bool': 1, 'char': 1,
            'size_t': 8,
            'DasGuid': 16,
            'DasResult': 4,
            'DasBool': 1,
        }
        # Load external enum/struct definitions from imported IDL files only
        self.all_interfaces = list(self.document.interfaces)
        if idl_file_path:
            imported_docs = resolve_import_chain(document, idl_file_path)
            self.type_mapper.load_external_definitions(list(imported_docs.values()))
            # Load interfaces from imported IDL files for cross-IDL UUID resolution
            for imp_doc in imported_docs.values():
                for iface in imp_doc.interfaces:
                    if not any(existing.name == iface.name for existing in self.all_interfaces):
                        self.all_interfaces.append(iface)

    def _collect_all_methods(self, interface: InterfaceDef) -> list:
        """收集接口及其所有父接口的方法（深度优先，从根到叶）"""
        iface_map = {iface.name: iface for iface in self.all_interfaces}

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
            for method in properties_to_methods(iface.properties):
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
        includes.append("#include <das/IDasBase.h>")
        includes.append("#include <das/Core/IPC/IStubBase.h>")
        includes.append("#include <das/Core/IPC/MemorySerializer.h>")
        includes.append("#include <das/Core/IPC/DistributedObjectManager.h>")
        includes.append("#include <das/Core/IPC/InterfaceParamSerialization.h>")
        includes.append("#include <das/Core/IPC/ProxyFactory.h>")
        includes.append("#include <das/Core/IPC/Serializer.h>")
        includes.append("#include <das/DasString.hpp>")
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
            # "IDasReadOnlyGuidVector",  # 需要 include IDasGuidVector.h
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

        # Check if any method has [in] interface pointer params (for DasProxyBase.h)
        has_in_interface_ptr = False

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
                    # Check if this is an [in] interface pointer (for DasProxyBase.h include)
                    if param.direction != ParamDirection.OUT:
                        has_in_interface_ptr = True

        # Add DasProxyBase.h if interface has [in] interface pointer params
        # (needed for CreateProxyByInterfaceId in remote branch)
        if has_in_interface_ptr:
            includes.add("das/Core/IPC/DasProxyBase.h")

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
        # 使用 type_kind 检测接口类型
        if type_info.is_pointer:
            if type_info.type_kind == TypeKind.INTERFACE:
                # Interface: IDL level is 1 star (pointer_level is ignored for interfaces)
                result += "*"
            elif type_info.pointer_level > 1:
                # Non-interface pointer: IDL level is (pointer_level - 1) stars
                # e.g., unsigned char** (pointer_level=2) -> unsigned char*
                result += "*" * (type_info.pointer_level - 1)

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

        if not all_methods:
            # Empty interface -- skip switch to avoid MSVC C4065 (no case labels)
            lines.append(f"{indent}DasResult DispatchMethod(")
            lines.append(f"{indent}    uint16_t method_id,")
            lines.append(f"{indent}    void* impl,")
            lines.append(f"{indent}    const uint8_t* params,")
            lines.append(f"{indent}    size_t params_size,")
            lines.append(f"{indent}    Das::Core::IPC::StubContext& ctx,")
            lines.append(f"{indent}    std::vector<uint8_t>& out_response) override")
            lines.append(f"{indent}{{")
            lines.append(f"{inner_indent}(void)method_id;")
            lines.append(f"{inner_indent}(void)impl;")
            lines.append(f"{inner_indent}(void)params;")
            lines.append(f"{inner_indent}(void)params_size;")
            lines.append(f"{inner_indent}(void)ctx;")
            lines.append(f"{inner_indent}(void)out_response;")
            lines.append(f"{inner_indent}return DAS_E_IPC_UNKNOWN_METHOD;")
            lines.append(f"{indent}}}")
            return "\n".join(lines)

        lines.append(f"{indent}DasResult DispatchMethod(")
        lines.append(f"{indent}    uint16_t method_id,")
        lines.append(f"{indent}    void* impl,")
        lines.append(f"{indent}    const uint8_t* params,")
        lines.append(f"{indent}    size_t params_size,")
        lines.append(f"{indent}    Das::Core::IPC::StubContext& ctx,")
        lines.append(f"{indent}    std::vector<uint8_t>& out_response) override")
        lines.append(f"{indent}{{")
        lines.append(f"{inner_indent}auto* target = static_cast<{interface.name}*>(impl);")
        lines.append(f"{inner_indent}if (!target) {{ return DAS_E_INVALID_POINTER; }}")
        lines.append(f"{inner_indent}switch (method_id)")
        lines.append(f"{inner_indent}{{")

        for method, _ in all_methods:
            lines.append(f"{inner_indent}case METHOD_{method.name.upper()}:")
            lines.append(f"{inner_indent}    return Handle{method.name}(target, params, params_size, ctx, out_response);")

        lines.append(f"{inner_indent}default:")
        lines.append(f"{inner_indent}    return DAS_E_IPC_UNKNOWN_METHOD;")
        lines.append(f"{inner_indent}}}")
        lines.append(f"{indent}}}")

        return "\n".join(lines)
    
    def _generate_handle_method_declaration(self, interface: InterfaceDef, method: MethodDef, namespace_depth: int = 0) -> str:
        """生成 Handle 方法声明（实例方法）"""
        indent = "    " * (namespace_depth + 1)
        return f"{indent}DasResult Handle{method.name}({interface.name}* impl, const uint8_t* params, size_t params_size, Das::Core::IPC::StubContext& ctx, std::vector<uint8_t>& out_response);"
    
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
        lines.append(f"inline DasResult {full_class_name}::Handle{method.name}({interface.name}* impl, const uint8_t* params, size_t params_size, Das::Core::IPC::StubContext& ctx, std::vector<uint8_t>& out_response)")
        lines.append(f"{{")
        lines.append(f"{inner_indent}(void)impl;")
        lines.append(f"{inner_indent}(void)params;")
        lines.append(f"{inner_indent}(void)params_size;")
        lines.append(f"{inner_indent}(void)ctx;")
        lines.append(f"{inner_indent}(void)out_response;")
        lines.append(f"{inner_indent}auto& object_manager = ctx.object_manager;")
        lines.append(f"{inner_indent}(void)object_manager;")

        return_type = method.return_type.base_type
        has_return = return_type != 'void'

        in_params = [p for p in method.parameters if p.direction != ParamDirection.OUT]
        out_params = [p for p in method.parameters if p.direction in (ParamDirection.OUT, ParamDirection.INOUT)]

        has_request_body = bool(in_params)

        lines.append(f"{inner_indent}DasResult serial_result = DAS_S_OK;")
        lines.append(f"{inner_indent}(void)serial_result;")

        # Handle 方法参数名列表（用于检测变量名冲突）
        handle_param_names = {'impl', 'params', 'params_size', 'out_response', 'ctx'}

        # 构建完整类型名（带指针）用于接口检测
        def _get_full_type_name(type_info):
            base = type_info.base_type
            if type_info.is_pointer:
                base += "*" * type_info.pointer_level
            return base

        # ===== 声明阶段：所有变量声明必须在反序列化代码之前 =====

        # 为 [in] 接口指针参数生成声明
        # [in] 接口指针需要声明，因为反序列化代码会赋值给它
        # 注意：跳过 IDasReadOnlyString，因为它在 _generate_deserialize_param_for_stub 中单独处理
        for param in in_params:
            if not param.type_info.is_pointer:
                continue
            # 跳过 IDasReadOnlyString - 在 deserialization 中单独处理
            if param.type_info.base_type == "IDasReadOnlyString":
                continue
            # 使用 type_kind 检测接口类型
            is_interface = (param.type_info.type_kind == TypeKind.INTERFACE
                            and param.type_info.is_pointer)
            if not is_interface:
                continue
            # 接口指针参数：声明为 IDL 层类型（单指针）
            local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name
            base_cpp = self._get_cpp_type_base(param.type_info)  # e.g., IDasImage*
            # Add namespace prefix for interface types
            interface_ns = self.type_mapper.interface_namespaces.get(param.type_info.base_type, "")
            if interface_ns:
                full_cpp_type = f"{interface_ns}::{base_cpp}"
            else:
                full_cpp_type = base_cpp
            lines.append(f"{inner_indent}{full_cpp_type} {local_name} = {{}};")

        for param in out_params:
            # 处理变量名冲突：INOUT 参数可能与 Handle 方法参数名冲突
            local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name

            # For [out] interface params: IDL gives IDasBinaryBuffer*, ABI expects IDasBinaryBuffer**
            # Declare with IDL-level pointer count (one less than ABI), pass &var to impl
            # 使用 type_kind 检测接口类型
            is_interface = (param.type_info.type_kind == TypeKind.INTERFACE
                            and param.type_info.is_pointer)
            if param.direction in (ParamDirection.OUT, ParamDirection.INOUT) and is_interface:
                # Declare with one less pointer level (IDL level)
                base_cpp = self._get_cpp_type_base(param.type_info)  # IDasBinaryBuffer*
                # Add namespace prefix for interface types
                interface_ns = self.type_mapper.interface_namespaces.get(param.type_info.base_type, "")
                if interface_ns:
                    full_cpp_type = f"{interface_ns}::{base_cpp}"
                else:
                    full_cpp_type = base_cpp
                lines.append(f"{inner_indent}{full_cpp_type} {local_name} = {{}};")
            elif param.direction in (ParamDirection.OUT, ParamDirection.INOUT) and param.type_info.is_pointer and param.type_info.pointer_level >= 1:
                # Non-interface [out] pointer: strip pointer for declaration, pass with &
                base_cpp = self._get_cpp_type_base(param.type_info)
                lines.append(f"{inner_indent}{base_cpp} {local_name} = {{}};")
            else:
                cpp_type = self._get_cpp_type(param.type_info)
                lines.append(f"{inner_indent}{cpp_type} {local_name} = {{}};")

        # ===== 反序列化阶段 =====
        if has_request_body:
            lines.append(f"{inner_indent}Das::Core::IPC::MemorySerializerReader reader(params, params_size);")
            lines.append("")

            # 反序列化参数（V3 Body Header 已在 IStubBase::HandleMessage 中解析）
            lines.append(f"{inner_indent}// Parameters (V3 Body after 16-byte header)")
            for param in in_params:
                deserialize_code = self._generate_deserialize_param_for_stub(param, inner_indent)
                for line in deserialize_code:
                    lines.append(f"{line}")
            lines.append("")

        call_params = []
        for param in method.parameters:
            # 处理变量名冲突
            local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name

            if param.direction in (ParamDirection.OUT, ParamDirection.INOUT):
                # For interface [out] params: ABI expects extra * so always pass &var
                is_iface = (param.type_info.type_kind == TypeKind.INTERFACE
                            and param.type_info.is_pointer)
                if is_iface:
                    call_params.append(f"&{local_name}")
                elif param.type_info.is_pointer and param.type_info.pointer_level >= 1:
                    # Non-interface [out] pointer: pass with &
                    call_params.append(f"&{local_name}")
                else:
                    call_params.append(f"&{local_name}")
            else:
                # [in] parameters: struct pointers (non-interface) need & prefix
                # e.g., const DasRect* - deserialize into local struct, pass &local_name
                is_interface = (param.type_info.type_kind == TypeKind.INTERFACE
                                and param.type_info.is_pointer)
                if param.type_info.is_pointer and param.type_info.pointer_level >= 1 and not is_interface:
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

        # ===== Response serialization =====
        if has_return or out_params:
            is_binary_buffer = method.attributes.get('binary_buffer', False)

            if is_binary_buffer:
                # Binary buffer serialization path
                resp_lines = self._generate_binary_buffer_response(
                    interface, method, out_params, has_return, inner_indent)
                lines.extend(resp_lines)
                lines.append(f"{inner_indent}out_response = writer.GetBuffer();")
            elif self._is_fixed_size_response(method):
                # Zero-heap-allocation: #pragma pack struct
                resp_lines = self._generate_fixed_size_response_body(
                    interface, method, out_params, has_return, inner_indent)
                lines.extend(resp_lines)
            else:
                # Variable-size: writer.Reserve() single allocation
                resp_lines = self._generate_variable_size_response_body(
                    interface, method, out_params, has_return, inner_indent)
                lines.extend(resp_lines)
                lines.append(f"{inner_indent}out_response = writer.GetBuffer();")
        # else: void method without out_params - no response body

        lines.append(f"{inner_indent}return DAS_S_OK;")
        lines.append(f"}}")

        return "\n".join(lines)

    def _is_fixed_size_response(self, method: MethodDef) -> bool:
        """Check if all [out] params + return value are fixed-size (no strings)

        NOTE: [out] interface pointers ARE fixed-size (12 bytes each).
        NOTE: [binary_buffer] methods are NOT fixed-size (use binary buffer path).
        """
        if method.attributes.get('binary_buffer', False):
            return False
        FIXED_SIZE_SPECIAL = {'DasGuid', 'DasResult', 'DasBool'}
        for param in method.parameters:
            if param.direction not in (ParamDirection.OUT, ParamDirection.INOUT):
                continue
            bt = param.type_info.base_type
            # IDasReadOnlyString [out] is variable-length
            if bt == 'IDasReadOnlyString' or bt == 'string':
                return False
            # Basic types
            if bt in self.type_mapper.TYPE_MAP:
                continue
            if bt in FIXED_SIZE_SPECIAL:
                continue
            # Enum
            if bt in self.type_mapper.enum_types:
                continue
            # Struct (all fields fixed)
            if bt in self.type_mapper.struct_types:
                continue
            # Interface pointer [out]: 12 bytes (session_id 2B + generation 2B + local_id 4B + interface_id 4B)
            if param.type_info.type_kind == TypeKind.INTERFACE and param.type_info.is_pointer:
                continue
            # Unknown type — not fixed-size
            return False
        return True

    def _get_out_param_fixed_size(self, param: ParameterDef) -> int:
        """Get byte size for a fixed-size [out] parameter.

        Interface pointers [out] = 12 bytes (session_id 2B + generation 2B + local_id 4B + interface_id 4B).
        """
        bt = param.type_info.base_type
        if bt in self.FIXED_SIZES:
            return self.FIXED_SIZES[bt]
        if bt in self.type_mapper.enum_types:
            return 4
        if bt in self.type_mapper.struct_defs:
            struct_def = self.type_mapper.struct_defs[bt]
            total = 0
            for field in struct_def.fields:
                total += self.FIXED_SIZES.get(field.type_name, 0)
            return total
        # Interface pointer [out]: 12 bytes
        if param.type_info.type_kind == TypeKind.INTERFACE and param.type_info.is_pointer:
            return 12
        return 0

    def _generate_response_struct_fields(self, param: ParameterDef, indent: str, local_name: str) -> List[str]:
        """Generate packed struct field declarations for an [out] parameter."""
        lines = []
        bt = param.type_info.base_type

        # Check if it's an interface pointer [out] using type_kind
        is_interface = (param.type_info.type_kind == TypeKind.INTERFACE
                        and param.type_info.is_pointer)

        if is_interface:
            # Interface pointer [out]: 4 fields for ObjectId + interface_id
            lines.append(f"{indent}uint16_t {local_name}_session_id;")
            lines.append(f"{indent}uint16_t {local_name}_generation;")
            lines.append(f"{indent}uint32_t {local_name}_local_id;")
            lines.append(f"{indent}uint32_t {local_name}_interface_id;")
            return lines

        # Enum: int32_t field
        if bt in self.type_mapper.enum_types:
            lines.append(f"{indent}int32_t {local_name};")
            return lines

        # Struct: inline fields per member
        if bt in self.type_mapper.struct_defs:
            struct_def = self.type_mapper.struct_defs[bt]
            if struct_def and struct_def.fields:
                for field in struct_def.fields:
                    field_type_info = self.type_mapper.get_type_info(field.type_name)
                    if field_type_info:
                        field_cpp_type, _, _, _ = field_type_info
                        lines.append(f"{indent}{field_cpp_type} {local_name}_{field.name};")
                    else:
                        # Fallback: use field type name directly (already a valid C++ type)
                        lines.append(f"{indent}{field.type_name} {local_name}_{field.name};")
            else:
                # Empty struct - use int32_t as placeholder
                lines.append(f"{indent}int32_t {local_name}_dummy;")
            return lines

        # Basic types
        type_info = self.type_mapper.get_type_info(bt)
        if type_info:
            cpp_type, _, _, _ = type_info
            lines.append(f"{indent}{cpp_type} {local_name};")
        else:
            lines.append(f"{indent}int32_t {local_name};")
        return lines

    def _generate_response_struct_fill(self, param: ParameterDef, prefix: str, indent: str, local_name: str) -> List[str]:
        """Generate code to fill a packed struct field for an [out] parameter."""
        lines = []
        bt = param.type_info.base_type

        # Check if it's an interface pointer [out] using type_kind
        is_interface = (param.type_info.type_kind == TypeKind.INTERFACE
                        and param.type_info.is_pointer)

        if is_interface:
            # Interface pointer [out]: find interface UUID and compute FNV-1a hash
            interface_name = self.type_mapper.get_interface_name(bt)
            interface_uuid = None
            if hasattr(self, 'document') and self.document:
                for iface in self.document.interfaces:
                    iface_short = iface.name.split("::")[-1]
                    if iface_short == interface_name or iface.name == bt:
                        interface_uuid = iface.uuid
                        break
            if interface_uuid is None and hasattr(self, 'all_interfaces'):
                for iface in self.all_interfaces:
                    iface_short = iface.name.split("::")[-1]
                    if iface_short == interface_name or iface.name == bt:
                        interface_uuid = iface.uuid
                        break

            if interface_uuid:
                iface_id_hash = fnv1a_hash_guid(interface_uuid)
            else:
                # Built-in types not in document (e.g. IDasBase): use BUILTIN_INTERFACE_HASHES
                iface_id_hash = BUILTIN_INTERFACE_HASHES.get(interface_name, 0)

            # Inline null check + RegisterLocalObject + 4-field assignment
            lines.append(f"{indent}if ({local_name} != nullptr)")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    Das::Core::IPC::ObjectId {local_name}_oid;")
            lines.append(f"{indent}    serial_result = object_manager.RegisterLocalObject({local_name}, {local_name}_oid);")
            lines.append(f"{indent}    if (DAS::IsFailed(serial_result))")
            lines.append(f"{indent}    {{")
            lines.append(f"{indent}        return serial_result;")
            lines.append(f"{indent}    }}")
            lines.append(f"{indent}    {prefix}{local_name}_session_id = {local_name}_oid.session_id;")
            lines.append(f"{indent}    {prefix}{local_name}_generation = {local_name}_oid.generation;")
            lines.append(f"{indent}    {prefix}{local_name}_local_id = {local_name}_oid.local_id;")
            lines.append(f"{indent}    {prefix}{local_name}_interface_id = 0x{iface_id_hash:08X}u;")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}// else: struct is zero-initialized, all fields already 0")
            return lines

        # Enum: static_cast<int32_t>
        if bt in self.type_mapper.enum_types:
            lines.append(f"{indent}{prefix}{local_name} = static_cast<int32_t>({local_name});")
            return lines

        # Struct: field-by-field assignment
        if bt in self.type_mapper.struct_defs:
            struct_def = self.type_mapper.struct_defs[bt]
            if struct_def and struct_def.fields:
                for field in struct_def.fields:
                    lines.append(f"{indent}{prefix}{local_name}_{field.name} = {local_name}.{field.name};")
            return lines

        # Basic types: direct assignment
        lines.append(f"{indent}{prefix}{local_name} = {local_name};")
        return lines

    def _generate_fixed_size_response_body(self, interface: InterfaceDef, method: MethodDef, out_params, has_return: bool, indent: str) -> List[str]:
        """Generate zero-alloc #pragma pack response struct for fixed-size responses."""
        lines = []
        struct_name = f"{method.name}_ResponseBody"
        handle_param_names = {'impl', 'params', 'params_size', 'out_response', 'ctx'}

        # Generate struct definition
        lines.append(f"{indent}#pragma pack(push, 1)")
        lines.append(f"{indent}struct {struct_name} {{")
        lines.append(f"{indent}    int32_t remote_result;")

        for param in out_params:
            local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name
            fields = self._generate_response_struct_fields(param, indent + "    ", local_name)
            lines.extend(fields)

        is_das_result_return = method.return_type.base_type in ('DasResult', 'int32_t')
        if has_return and not is_das_result_return:
            lines.append(f"{indent}    int32_t return_value;")

        lines.append(f"{indent}}};")
        lines.append(f"{indent}#pragma pack(pop)")
        lines.append("")

        # Create instance and fill
        lines.append(f"{indent}{struct_name} resp{{}};")

        if has_return:
            lines.append(f"{indent}resp.remote_result = static_cast<int32_t>(call_result);")
        else:
            lines.append(f"{indent}resp.remote_result = static_cast<int32_t>(DAS_S_OK);")

        # Fill out params only if impl call succeeded (output params may be NULL/invalid on failure)
        if has_return and is_das_result_return:
            lines.append(f"{indent}if (DAS::IsOk(call_result))")
            lines.append(f"{indent}{{")

            fill_indent = indent + "    "
            for param in out_params:
                local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name
                fill_lines = self._generate_response_struct_fill(param, "resp.", fill_indent, local_name)
                lines.extend(fill_lines)

            lines.append(f"{indent}}}")
            # else: struct is zero-initialized, all fields already 0
        else:
            # No DasResult return — always fill out params
            for param in out_params:
                local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name
                fill_lines = self._generate_response_struct_fill(param, "resp.", indent, local_name)
                lines.extend(fill_lines)

        if has_return and not is_das_result_return:
            lines.append(f"{indent}resp.return_value = static_cast<int32_t>(call_result);")

        lines.append("")
        lines.append(f"{indent}out_response.assign(")
        lines.append(f"{indent}    reinterpret_cast<const uint8_t*>(&resp),")
        lines.append(f"{indent}    reinterpret_cast<const uint8_t*>(&resp) + sizeof(resp));")

        return lines

    def _generate_variable_size_response_body(self, interface: InterfaceDef, method: MethodDef, out_params, has_return: bool, indent: str) -> List[str]:
        """Generate response body with writer.Reserve() for single allocation."""
        lines = []
        handle_param_names = {'impl', 'params', 'params_size', 'out_response', 'ctx'}

        is_das_result_return = method.return_type.base_type in ('DasResult', 'int32_t')

        # If method returns DasResult, check call_result before accessing output params
        # If the call failed, output params may be NULL/invalid — skip serialization and
        # only write the error code in the response.
        if has_return and is_das_result_return:
            lines.append(f"{indent}// If impl call failed, skip output param serialization (params may be NULL)")
            lines.append(f"{indent}if (DAS::IsFailed(call_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    Das::Core::IPC::MemorySerializerWriter writer;")
            lines.append(f"{indent}    serial_result = writer.WriteInt32(call_result);")
            lines.append(f"{indent}    if (DAS::IsFailed(serial_result))")
            lines.append(f"{indent}    {{")
            lines.append(f"{indent}        return serial_result;")
            lines.append(f"{indent}    }}")
            lines.append(f"{indent}    out_response = writer.GetBuffer();")
            lines.append(f"{indent}    return DAS_S_OK;")
            lines.append(f"{indent}}}")
            lines.append("")

        # Phase 1: Pre-calculate total response size
        lines.append(f"{indent}// Pre-calculate total response size for single allocation")
        lines.append(f"{indent}size_t total_response_size = 4;  // remote_result (int32)")

        if has_return and not is_das_result_return:
            lines.append(f"{indent}total_response_size += 4;  // return_value (int32)")

        string_params = []  # Track IDasReadOnlyString [out] params for GetUtf8
        for param in out_params:
            local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name
            bt = param.type_info.base_type
            if bt == 'IDasReadOnlyString' and param.type_info.is_pointer:
                # Variable-length: GetUtf8 + strlen
                lines.append(f"{indent}const char* {local_name}_utf8_tmp;")
                lines.append(f"{indent}serial_result = {local_name}->GetUtf8(&{local_name}_utf8_tmp);")
                lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
                lines.append(f"{indent}{{")
                lines.append(f"{indent}    return serial_result;")
                lines.append(f"{indent}}}")
                lines.append(f"{indent}const size_t {local_name}_len = std::strlen({local_name}_utf8_tmp);")
                lines.append(f"{indent}total_response_size += 8 + {local_name}_len;  // uint64 length prefix + string data")
                string_params.append(local_name)
            else:
                size = self._get_out_param_fixed_size(param)
                lines.append(f"{indent}total_response_size += {size};  // {param.name}")

        lines.append("")

        # Phase 2: Create writer with pre-allocation
        lines.append(f"{indent}Das::Core::IPC::MemorySerializerWriter writer;")
        lines.append(f"{indent}writer.Reserve(total_response_size);")
        lines.append("")

        # Phase 3: Write response body
        # Write remote_result
        if has_return:
            lines.append(f"{indent}serial_result = writer.WriteInt32(call_result);")
        else:
            lines.append(f"{indent}serial_result = writer.WriteInt32(DAS_S_OK);")
        lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return serial_result;")
        lines.append(f"{indent}}}")
        lines.append("")

        # Write out_params
        for param in out_params:
            local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name
            bt = param.type_info.base_type
            if bt == 'IDasReadOnlyString' and param.type_info.is_pointer:
                # String: write length prefix + data
                lines.append(f"{indent}serial_result = writer.WriteBytes(reinterpret_cast<const uint8_t*>({local_name}_utf8_tmp), {local_name}_len);")
                lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
                lines.append(f"{indent}{{")
                lines.append(f"{indent}    return serial_result;")
                lines.append(f"{indent}}}")
            else:
                serialize_code = self._generate_serialize_param_for_stub(param, indent, local_name)
                for line in serialize_code:
                    lines.append(f"{line}")

        # Write return_value (only for non-DasResult return types)
        if has_return and not is_das_result_return:
            serialize_return_code = self._generate_serialize_return_for_stub(method.return_type, indent)
            for line in serialize_return_code:
                lines.append(f"{line}")

        return lines

    def _generate_binary_buffer_response(self, interface: InterfaceDef, method: MethodDef, out_params, has_return: bool, indent: str) -> List[str]:
        """Generate response body for [binary_buffer] methods.

        Wire format (pipe path): int32_t remote_result | uint64_t data_size (via WriteBytes) | uint8_t data[data_size]
        Wire format (SHM path):  int32_t remote_result | uint64_t shm_handle | uint64_t data_size
        LARGE_MESSAGE_THRESHOLD determines which path is used.
        """
        lines = []

        # Step 1: Get buffer size by calling impl->GetSize
        lines.append(f"{indent}// Binary buffer: get size first")
        lines.append(f"{indent}uint64_t binary_data_size = 0;")
        lines.append(f"{indent}DasResult size_result = impl->GetSize(&binary_data_size);")
        lines.append(f"{indent}if (DAS::IsFailed(size_result))")
        lines.append(f"{indent}{{")
        lines.append(f'{indent}    DAS_CORE_LOG_WARN("Stub: GetSize failed, result={{}}", size_result);')
        lines.append(f"{indent}    binary_data_size = 0;")
        lines.append(f"{indent}}}")
        lines.append("")

        # Step 2: Find the unsigned char** out param (the data pointer)
        # The impl->{method_name}(...) call was already generated by the caller.
        # We need to identify the local variable name for the data pointer.
        data_param = None
        handle_param_names = {'impl', 'params', 'params_size', 'out_response', 'ctx'}
        for param in out_params:
            bt = param.type_info.base_type
            if bt in ('unsigned char', 'uint8_t') and param.type_info.is_pointer:
                data_param = param
                break

        if data_param is None:
            lines.append(f"{indent}// ERROR: [binary_buffer] method has no unsigned char* [out] param")
            lines.append(f"{indent}return DAS_E_NOT_IMPLEMENTED;")
            return lines

        data_local_name = f"arg_{data_param.name}" if data_param.name in handle_param_names else data_param.name

        # Step 3: SHM path for large buffers (only when call_result is OK)
        lines.append(f"{indent}// SHM path: allocate shared memory block for large buffers")
        lines.append(f"{indent}if (DAS::IsOk(call_result) && binary_data_size >= Das::Core::IPC::LARGE_MESSAGE_THRESHOLD)")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    auto* conn_mgr = ctx.run_loop.GetConnectionManager();")
        lines.append(f"{indent}    if (conn_mgr)")
        lines.append(f"{indent}    {{")
        lines.append(f"{indent}        Das::Core::IPC::ConnectionInfo conn_info;")
        lines.append(f"{indent}        if (DAS::IsOk(conn_mgr->GetConnection(ctx.header.GetSourceSessionId(), conn_info))")
        lines.append(f"{indent}            && conn_info.shm_pool != nullptr)")
        lines.append(f"{indent}        {{")
        lines.append(f"{indent}            Das::Core::IPC::SharedMemoryBlock shm_block;")
        lines.append(f"{indent}            if (DAS::IsOk(conn_info.shm_pool->Allocate(")
        lines.append(f"{indent}                    static_cast<size_t>(binary_data_size), shm_block)))")
        lines.append(f"{indent}            {{")
        lines.append(f"{indent}                std::memcpy(shm_block.data, {data_local_name}, static_cast<size_t>(binary_data_size));")
        lines.append(f"{indent}                Das::Core::IPC::MemorySerializerWriter shm_writer;")
        lines.append(f"{indent}                shm_writer.Reserve(4 + 8 + 8);")
        if has_return:
            lines.append(f"{indent}                shm_writer.WriteInt32(static_cast<int32_t>(call_result));")
        else:
            lines.append(f"{indent}                shm_writer.WriteInt32(static_cast<int32_t>(DAS_S_OK));")
        lines.append(f"{indent}                shm_writer.WriteUInt64(shm_block.handle);")
        lines.append(f"{indent}                shm_writer.WriteUInt64(binary_data_size);")
        lines.append(f"{indent}                out_response = shm_writer.GetBuffer();")
        lines.append(f"{indent}                ctx.response_flags = Das::Core::IPC::MessageFlags::SHM_RESPONSE;")
        lines.append(f"{indent}                return DAS_S_OK;")
        lines.append(f"{indent}            }}")
        lines.append(f'{indent}            DAS_CORE_LOG_WARN("Stub: SHM Allocate failed, falling back to pipe (size={{}})", binary_data_size);')
        lines.append(f"{indent}        }}")
        lines.append(f'{indent}        else')
        lines.append(f'{indent}        {{')
        lines.append(f'{indent}            DAS_CORE_LOG_WARN("Stub: SHM connection not found for source_session={{}}, falling back to pipe", ctx.header.GetSourceSessionId());')
        lines.append(f'{indent}        }}')
        lines.append(f"{indent}    }}")
        lines.append(f"{indent}}}")
        lines.append("")

        # Step 4: Pipe path (small buffer or SHM fallback)
        lines.append(f"{indent}// Pipe path: serialize data directly in response body")
        lines.append(f"{indent}const size_t total_response_size = 4 + 8 + static_cast<size_t>(binary_data_size);")
        lines.append(f"{indent}Das::Core::IPC::MemorySerializerWriter writer;")
        lines.append(f"{indent}writer.Reserve(total_response_size);")
        lines.append("")

        # Step 5: Write remote_result (reuse serial_result from function scope)
        if has_return:
            lines.append(f"{indent}serial_result = writer.WriteInt32(static_cast<int32_t>(call_result));")
        else:
            lines.append(f"{indent}serial_result = writer.WriteInt32(static_cast<int32_t>(DAS_S_OK));")
        lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return serial_result;")
        lines.append(f"{indent}}}")

        # Step 6: Write binary data (WriteBytes includes uint64 size prefix)
        lines.append(f"{indent}if (DAS::IsOk(call_result) && {data_local_name} != nullptr)")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    serial_result = writer.WriteBytes(reinterpret_cast<const uint8_t*>({data_local_name}), static_cast<size_t>(binary_data_size));")
        lines.append(f"{indent}}}")
        lines.append(f"{indent}else")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    serial_result = writer.WriteBytes(nullptr, 0);")
        lines.append(f"{indent}}}")
        lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
        lines.append(f"{indent}{{")
        lines.append(f"{indent}    return serial_result;")
        lines.append(f"{indent}}}")

        return lines

    def _generate_deserialize_param_for_stub(self, param: ParameterDef, indent: str) -> List[str]:
        """生成参数反序列化代码（用于 Stub 从请求体读取参数）"""
        lines = []

        # Handle 方法参数名列表（用于检测变量名冲突）
        handle_param_names = {'impl', 'params', 'params_size', 'out_response', 'ctx'}
        local_name = f"arg_{param.name}" if param.name in handle_param_names else param.name

        # 构建完整类型名（带指针）用于接口检测
        full_type_name = param.type_info.base_type
        if param.type_info.is_pointer:
            full_type_name += "*" * param.type_info.pointer_level

        # IDasReadOnlyString special handling (zero-copy) - BEFORE interface check
        # IDasReadOnlyString is a concrete type (not interface), needs explicit declaration
        # Uses ReadStringView + CreateIDasReadOnlyStringFromUtf8WithLength for zero heap allocation
        if param.type_info.base_type == "IDasReadOnlyString":
            lines.append(f"{indent}// 反序列化 IDasReadOnlyString (zero-copy)")
            # Declare the pointer variable (IDasReadOnlyString* for [in] params)
            lines.append(f"{indent}IDasReadOnlyString* {local_name} = {{}};")
            lines.append(f"{indent}const char* {local_name}_ptr = nullptr;")
            lines.append(f"{indent}size_t {local_name}_len = 0;")
            lines.append(f"{indent}serial_result = reader.ReadStringView(&{local_name}_ptr, &{local_name}_len);")
            lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return serial_result;")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}serial_result = ::CreateIDasReadOnlyStringFromUtf8WithLength({local_name}_ptr, {local_name}_len, &{local_name});")
            lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return serial_result;")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}if (!{local_name})")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return DAS_E_OUT_OF_MEMORY;")
            lines.append(f"{indent}}}")
            return lines

        # 检查是否是接口指针类型（使用 type_kind）
        is_interface_for_deser = (param.type_info.type_kind == TypeKind.INTERFACE
                                  and param.type_info.is_pointer)
        if is_interface_for_deser:
            interface_name = self.type_mapper.get_interface_name(param.type_info.base_type)
            proxy_name = f"{interface_name}Proxy"
            param_name = local_name

            # 获取接口的命名空间前缀
            interface_ns = self.type_mapper.interface_namespaces.get(param.type_info.base_type, "")
            if interface_ns:
                full_interface_name = f"{interface_ns}::{interface_name}"
            else:
                full_interface_name = interface_name

            # 反序列化接口指针：先读 ObjectId，再判断本地/远程
            lines.append(f"{indent}// 反序列化接口指针: {interface_name}*")
            lines.append(f"{indent}uint64_t {param_name}_encoded_value;")
            lines.append(f"{indent}serial_result = reader.ReadUInt64(&{param_name}_encoded_value);")
            lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return serial_result;")
            lines.append(f"{indent}}}")

            # Compute interface_id hash at generation time
            interface_uuid = None
            if hasattr(self, 'document') and self.document:
                for iface in self.document.interfaces:
                    iface_short = iface.name.split("::")[-1]
                    if iface_short == interface_name or iface.name == param.type_info.base_type:
                        interface_uuid = iface.uuid
                        break
            if interface_uuid is None and hasattr(self, 'all_interfaces'):
                for iface in self.all_interfaces:
                    iface_short = iface.name.split("::")[-1]
                    if iface_short == interface_name or iface.name == param.type_info.base_type:
                        interface_uuid = iface.uuid
                        break

            if interface_uuid:
                iface_id_hash = fnv1a_hash_guid(interface_uuid)
            else:
                # Built-in types not in document (e.g. IDasBase): use BUILTIN_INTERFACE_HASHES
                iface_id_hash = BUILTIN_INTERFACE_HASHES.get(interface_name, 0)

            lines.append(f"{indent}{{")
            lines.append(f"{indent}    IDasBase* {param_name}_base = nullptr;")
            lines.append(f"{indent}    serial_result = Das::Core::IPC::DeserializeInInterfaceParam(")
            lines.append(f"{indent}        {param_name}_encoded_value,")
            lines.append(f"{indent}        0x{iface_id_hash:08X}u,")
            lines.append(f"{indent}        ctx.object_manager,")
            lines.append(f"{indent}        ctx.run_loop,")
            lines.append(f"{indent}        ctx.business_thread,")
            lines.append(f"{indent}        ctx.proxy_factory,")
            lines.append(f"{indent}        &{param_name}_base);")
            lines.append(f"{indent}    if (DAS::IsFailed(serial_result))")
            lines.append(f"{indent}    {{")
            lines.append(f"{indent}        return serial_result;")
            lines.append(f"{indent}    }}")
            lines.append(f"{indent}    {param_name} = static_cast<{full_interface_name}*>({param_name}_base);")
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
            # Enum types: declare with enum type, read into temp int32_t, then cast
            if param.type_info.base_type in self.type_mapper.enum_types:
                lines.append(f"{indent}{param.type_info.base_type} {local_name};")
                lines.append(f"{indent}int32_t {local_name}_temp;")
                lines.append(f"{indent}serial_result = reader.{read_method}(&{local_name}_temp);")
                lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
                lines.append(f"{indent}{{")
                lines.append(f"{indent}    return serial_result;")
                lines.append(f"{indent}}}")
                lines.append(f"{indent}{local_name} = static_cast<{param.type_info.base_type}>({local_name}_temp);")
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

        # 使用 local_name 如果提供，否则使用 param.name
        var_name = local_name if local_name else param.name

        # IDasReadOnlyString special handling (Fix 4): use GetUtf8(const char**) for serialization
        # IDasReadOnlyString::GetUtf8(const char** out_string) allocates string via output parameter
        if param.type_info.base_type == "IDasReadOnlyString" and param.type_info.is_pointer and param.type_info.pointer_level >= 1:
            lines.append(f"{indent}const char* {var_name}_utf8_tmp;")
            lines.append(f"{indent}serial_result = {var_name}->GetUtf8(&{var_name}_utf8_tmp);")
            lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return serial_result;")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}serial_result = writer.WriteString({var_name}_utf8_tmp);")
            lines.append(f"{indent}if (DAS::IsFailed(serial_result))")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    return serial_result;")
            lines.append(f"{indent}}}")
            return lines

        # [out] interface pointer serialization: register object + write ObjectId + interface_id
        # 使用 type_kind 检测接口类型
        is_interface = (param.type_info.type_kind == TypeKind.INTERFACE
                        and param.type_info.is_pointer)

        if is_interface and param.type_info.is_pointer:
            # Get the interface name for looking up its UUID
            interface_name = self.type_mapper.get_interface_name(param.type_info.base_type)

            # Find the interface UUID to compute the FNV-1a hash at generation time
            interface_uuid = None
            if hasattr(self, 'document') and self.document:
                for iface in self.document.interfaces:
                    iface_short = iface.name.split("::")[-1]
                    if iface_short == interface_name or iface.name == param.type_info.base_type:
                        interface_uuid = iface.uuid
                        break
            # Also check all_interfaces if available
            if interface_uuid is None and hasattr(self, 'all_interfaces'):
                for iface in self.all_interfaces:
                    iface_short = iface.name.split("::")[-1]
                    if iface_short == interface_name or iface.name == param.type_info.base_type:
                        interface_uuid = iface.uuid
                        break

            if interface_uuid:
                lines.append(f"{indent}// 序列化 [out] 接口指针: {interface_name}*")
                lines.append(f"{indent}if ({var_name} != nullptr)")
                lines.append(f"{indent}{{")
                lines.append(f"{indent}    Das::Core::IPC::ObjectId {var_name}_oid;")
                lines.append(f"{indent}    // RegisterLocalObject 内部 DasPtr 构造函数自动 AddRef")
                lines.append(f"{indent}    serial_result = object_manager.RegisterLocalObject({var_name}, {var_name}_oid);")
                lines.append(f"{indent}    if (DAS::IsFailed(serial_result))")
                lines.append(f"{indent}    {{")
                lines.append(f"{indent}        return serial_result;")
                lines.append(f"{indent}    }}")
                lines.append(f"{indent}    serial_result = writer.WriteUInt16({var_name}_oid.session_id);")
                lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
                lines.append(f"{indent}    serial_result = writer.WriteUInt16({var_name}_oid.generation);")
                lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
                lines.append(f"{indent}    serial_result = writer.WriteUInt32({var_name}_oid.local_id);")
                lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
                lines.append(f"{indent}    serial_result = writer.WriteUInt32(0x{fnv1a_hash_guid(interface_uuid):08X}u);")
                lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
            else:
                # Built-in types not in document (e.g. IDasBase): use BUILTIN_INTERFACE_HASHES
                iface_id_hash = BUILTIN_INTERFACE_HASHES.get(interface_name, 0)

                lines.append(f"{indent}// 序列化 [out] 接口指针: {interface_name}*")
                lines.append(f"{indent}if ({var_name} != nullptr)")
                lines.append(f"{indent}{{")
                lines.append(f"{indent}    Das::Core::IPC::ObjectId {var_name}_oid;")
                lines.append(f"{indent}    serial_result = object_manager.RegisterLocalObject({var_name}, {var_name}_oid);")
                lines.append(f"{indent}    if (DAS::IsFailed(serial_result))")
                lines.append(f"{indent}    {{")
                lines.append(f"{indent}        return serial_result;")
                lines.append(f"{indent}    }}")
                lines.append(f"{indent}    serial_result = writer.WriteUInt16({var_name}_oid.session_id);")
                lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
                lines.append(f"{indent}    serial_result = writer.WriteUInt16({var_name}_oid.generation);")
                lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
                lines.append(f"{indent}    serial_result = writer.WriteUInt32({var_name}_oid.local_id);")
                lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
                lines.append(f"{indent}    serial_result = writer.WriteUInt32(0x{iface_id_hash:08X}u);")
                lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
            lines.append(f"{indent}}}")
            lines.append(f"{indent}else")
            lines.append(f"{indent}{{")
            lines.append(f"{indent}    // Null pointer: write zero ObjectId + zero interface_id")
            lines.append(f"{indent}    serial_result = writer.WriteUInt16(0);")
            lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
            lines.append(f"{indent}    serial_result = writer.WriteUInt16(0);")
            lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
            lines.append(f"{indent}    serial_result = writer.WriteUInt32(0);")
            lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
            lines.append(f"{indent}    serial_result = writer.WriteUInt32(0);")
            lines.append(f"{indent}    if (DAS::IsFailed(serial_result)) {{ return serial_result; }}")
            lines.append(f"{indent}}}")
            return lines

        type_info = self.type_mapper.get_type_info(param.type_info.base_type)

        if type_info is None:
            lines.append(f"{indent}// TODO: Serialize type {param.type_info.base_type}")
            return lines

        cpp_type, write_method, _, is_struct = type_info

        if is_struct:
            # 使用内联字段展开进行序列化
            struct_def = self.type_mapper.struct_defs.get(param.type_info.base_type)
            if struct_def and struct_def.fields:
                # For interface pointers: use -> because variable is a real pointer
                # For struct pointers: use . because variable is a value type (pointer was stripped for declaration)
                is_interface = (param.type_info.type_kind == TypeKind.INTERFACE
                                and param.type_info.is_pointer)
                accessor = "->" if is_interface else "."
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
                # Struct has no fields or struct_def is None: use direct write method
                # For non-interface types passed by pointer: variable is value type, don't dereference
                is_interface = (param.type_info.type_kind == TypeKind.INTERFACE
                                and param.type_info.is_pointer)
                if param.type_info.is_pointer and param.type_info.pointer_level >= 1 and is_interface:
                    lines.append(f"{indent}serial_result = writer.{write_method}(*{var_name});")
                else:
                    lines.append(f"{indent}serial_result = writer.{write_method}({var_name});")
        else:
            # Non-struct types (basic types, special types like DasGuid)
            # For interface pointers: dereference because variable is a real pointer
            # For non-interface pointers (e.g., DasGuid*): variable is value type, don't dereference
            is_interface = (param.type_info.type_kind == TypeKind.INTERFACE
                            and param.type_info.is_pointer)
            if param.type_info.is_pointer and param.type_info.pointer_level >= 1 and is_interface:
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

        lines.append(f"{indent}class {class_name} : public Das::Core::IPC::IStubBase")
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

            # Add SHM includes for [binary_buffer] methods
            has_binary_buffer = any(m.attributes.get('binary_buffer', False) for m in interface.methods)
            if has_binary_buffer:
                content += "#include <das/Core/IPC/AsyncIpcTransport.h>\n"
                content += "#include <das/Core/IPC/ConnectionManager.h>\n"
                content += "#include <das/Core/IPC/SharedMemoryPool.h>\n"

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
