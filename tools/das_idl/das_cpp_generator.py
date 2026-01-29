"""
DAS C++ 代码生成器
根据解析后的 IDL 定义生成 C++ 头文件

新方案：只生成原始 C++ 接口
- 不再生成 IDasSwig* 系列接口
- SWIG 相关配置通过单独的 .i 文件处理
"""

import os
from datetime import datetime, timezone
from typing import Optional, List
from das_idl_parser import (
    IdlDocument, InterfaceDef, EnumDef, MethodDef, PropertyDef,
    ParameterDef, TypeInfo, ParamDirection, ImportDef,
    StructDef, StructFieldDef
)


class CppTypeMapper:
    """C++ 类型映射器"""

    # IDL 类型到 C++ 类型的映射
    TYPE_MAP = {
        'bool': 'bool',
        'int8': 'int8_t',
        'int16': 'int16_t',
        'int32': 'int32_t',
        'int64': 'int64_t',
        'uint8': 'uint8_t',
        'uint16': 'uint16_t',
        'uint32': 'uint32_t',
        'uint64': 'uint64_t',
        'float': 'float',
        'double': 'double',
        'size_t': 'size_t',
        'int': 'int32_t',
        'uint': 'uint32_t',
        'DasResult': 'DasResult',
        'DasBool': 'DasBool',
        'DasGuid': 'DasGuid',
        'DasString': 'DasReadOnlyString',
        'DasReadOnlyString': 'DasReadOnlyString',
        'void': 'void',
    }

    @staticmethod
    def is_interface_type(type_name: str) -> bool:
        """判断是否是接口类型 (以 I 开头的类型)"""
        return type_name.startswith('I') and len(type_name) > 1 and type_name[1:2].isupper()

    @staticmethod
    def is_string_type(type_name: str) -> bool:
        """判断是否是字符串类型"""
        return type_name in ('DasString', 'DasReadOnlyString', 'IDasReadOnlyString', 'IDasString')

    @staticmethod
    def needs_dasstring_include(type_name: str) -> bool:
        """判断类型是否需要包含 DasString.hpp"""
        return type_name in ('DasReadOnlyString', 'DasString', 'IDasReadOnlyString', 'IDasString')

    @staticmethod
    def needs_dastypeinfo_include(type_name: str) -> bool:
        """判断类型是否需要包含 IDasTypeInfo.h"""
        return type_name == 'IDasTypeInfo'

    @classmethod
    def map_type(cls, type_info: TypeInfo) -> str:
        """将 IDL 类型转换为 C++ 类型"""
        base = cls.TYPE_MAP.get(type_info.base_type, type_info.base_type)

        result = ""

        if type_info.is_const:
            result += "const "

        # 字符串类型作为输入参数时，使用 IDasReadOnlyString*
        if cls.is_string_type(type_info.base_type) and not type_info.is_pointer and not type_info.is_reference:
            result += "IDasReadOnlyString*"
        else:
            result += base

        if type_info.is_pointer:
            result += "*" * type_info.pointer_level

        if type_info.is_reference:
            result += "&"

        return result

    @classmethod
    def get_out_param_type(cls, type_info: TypeInfo) -> str:
        """获取输出参数类型"""
        base = cls.TYPE_MAP.get(type_info.base_type, type_info.base_type)

        # 对于接口类型，输出参数是 IXxx**
        if cls.is_interface_type(type_info.base_type):
            return f"{base}**"
        # 对于字符串类型，输出参数是 IDasReadOnlyString**
        elif cls.is_string_type(type_info.base_type):
            return "IDasReadOnlyString**"
        else:
            # 对于其他类型，直接使用原始的 pointer_level
            # 例如：void** (pointer_level=2) -> void**
            return f"{base}{'*' * type_info.pointer_level}"


class CppCodeGenerator:
    """C++ 代码生成器 - 只生成原始 C++ 接口"""

    def __init__(self, document: IdlDocument, namespace: str = "", imported_documents: dict = None, global_type_map: dict = None, idl_file_name: str = None):
        self.document = document
        self.namespace = namespace
        self.indent = "    "  # 4 空格缩进
        self.imported_documents = imported_documents or {}  # 导入的IDL文档映射: {文件名: IdlDocument}
        self.idl_file_name = idl_file_name  # IDL文件名

        # 构建类型到命名空间的映射表（分层）
        if global_type_map:
            # 使用全局类型映射（批量模式）
            self.local_type_to_namespace = {}
            self.imported_type_to_namespace = global_type_map.copy()
            # 从全局映射中提取本地类型
            for interface in self.document.interfaces:
                self.local_type_to_namespace[interface.name] = interface.namespace
            for enum in self.document.enums:
                self.local_type_to_namespace[enum.name] = enum.namespace
            for struct in self.document.structs:
                self.local_type_to_namespace[struct.name] = struct.namespace
        else:
            # 构建本地类型映射（单文件模式）
            self.local_type_to_namespace = {}
            self.imported_type_to_namespace = {}
            self._build_type_namespace_map()

    def _build_type_namespace_map(self):
        """构建类型名到命名空间的映射表（分层），用于生成完全限定名称"""
        # 收集当前文档中定义的所有类型（本地类型）
        for interface in self.document.interfaces:
            self.local_type_to_namespace[interface.name] = interface.namespace

        for enum in self.document.enums:
            self.local_type_to_namespace[enum.name] = enum.namespace

        for struct in self.document.structs:
            self.local_type_to_namespace[struct.name] = struct.namespace

        # 收集导入文档中的类型（导入类型）
        for idl_file, imported_doc in self.imported_documents.items():
            for interface in imported_doc.interfaces:
                # 只在导入类型中不存在时才添加（避免覆盖）
                if interface.name not in self.imported_type_to_namespace:
                    self.imported_type_to_namespace[interface.name] = interface.namespace

            for enum in imported_doc.enums:
                if enum.name not in self.imported_type_to_namespace:
                    self.imported_type_to_namespace[enum.name] = enum.namespace

            for struct in imported_doc.structs:
                if struct.name not in self.imported_type_to_namespace:
                    self.imported_type_to_namespace[struct.name] = struct.namespace

    def _qualify_type_if_needed(self, type_name: str, current_namespace: str) -> str:
        """如果类型在不同命名空间，返回完全限定名称

        Args:
            type_name: 类型名称
            current_namespace: 当前所在的命名空间

        Returns:
            如果类型在不同命名空间，返回 ::Namespace::TypeName
            如果类型在相同命名空间，返回 TypeName（不添加任何前缀）
            如果类型没有命名空间，返回 ::TypeName
        """
        # 如果类型不是接口类型（不需要限定的基本类型），直接返回
        if not CppTypeMapper.is_interface_type(type_name):
            return type_name

        # 先在本地类型中查找
        type_namespace = self.local_type_to_namespace.get(type_name)
        if type_namespace is None:
            # 再在导入类型中查找
            type_namespace = self.imported_type_to_namespace.get(type_name, "")

        # 如果类型和当前在同一个命名空间，不需要限定（不添加任何前缀！）
        if type_namespace == current_namespace:
            return type_name

        # 如果类型有命名空间且与当前命名空间不同，返回完全限定名称
        if type_namespace:
            return f"::{type_namespace}::{type_name}"

        # 类型没有命名空间，但当前在命名空间内，需要用 :: 前缀
        if current_namespace:
            return f"::{type_name}"

        return type_name

    def _file_header(self, guard_name: str, import_includes: list = None, type_includes: set = None, forward_declarations: list = None) -> str:
        """生成文件头"""
        timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

        # 生成类型相关的 include 语句
        type_includes_str = ""
        if type_includes:
            type_includes_str = "\n".join(sorted(type_includes))
            type_includes_str = f"\n// Type includes\n{type_includes_str}\n"

        # 生成前向声明
        forward_declarations_str = ""
        if forward_declarations:
            forward_declarations_str = "\n".join(forward_declarations)
            forward_declarations_str = f"\n// Forward declarations\n{forward_declarations_str}\n"

        # 生成导入的 include 语句
        import_includes_str = ""
        if import_includes:
            import_includes_str = "\n".join(f'#include "{inc}"' for inc in import_includes)
            import_includes_str = f"\n// Imported IDL headers\n{import_includes_str}\n"

        # 生成IDL文件名注释
        idl_file_comment = ""
        if self.idl_file_name:
            idl_file_comment = f"// Source IDL file: {self.idl_file_name}\n"

        return f"""// This file is automatically generated by DAS IDL Generator
// Generated at: {timestamp}
{idl_file_comment}// !!! DO NOT EDIT !!!

#ifndef {guard_name}
#define {guard_name}

#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
{type_includes_str}
{forward_declarations_str}
{import_includes_str}
"""

    def _file_footer(self, guard_name: str) -> str:
        """生成文件尾"""
        return f"""
#endif // {guard_name}
"""

    def _to_upper_snake(self, name: str) -> str:
        """将 PascalCase 转换为 UPPER_SNAKE_CASE"""
        result = []
        for i, c in enumerate(name):
            if c.isupper() and i > 0:
                result.append('_')
            result.append(c.upper())
        return ''.join(result)

    def _generate_namespace_open(self, namespace: str) -> str:
        """生成命名空间开始标记，支持 C++17 嵌套命名空间语法"""
        if namespace:
            return f"namespace {namespace} {{\n"
        return ""

    def _generate_namespace_close(self, namespace: str) -> str:
        """生成命名空间结束标记"""
        if namespace:
            return f"}} // namespace {namespace}\n"
        return ""

    def _generate_uuid_define(self, interface: InterfaceDef, namespace: str = "") -> str:
        """生成 DAS_DEFINE_GUID 宏或 DAS_DEFINE_GUID_IN_NAMESPACE 宏"""
        if not interface.uuid:
            return ""

        # 解析 UUID 字符串，移除所有分隔符
        uuid = interface.uuid.replace("-", "")
        if len(uuid) != 32:
            return f"// Invalid UUID for {interface.name}: {interface.uuid}\n"

        # 分解 UUID 为各部分
        data1 = f"0x{uuid[0:8]}"
        data2 = f"0x{uuid[8:12]}"
        data3 = f"0x{uuid[12:16]}"
        data4 = [f"0x{uuid[i:i+2]}" for i in range(16, 32, 2)]

        # 生成 IID 名称: IDasXxx -> DAS_IID_XXX
        if interface.name.startswith("IDas"):
            iid_suffix = interface.name[4:]  # 去掉 IDas 前缀
        else:
            iid_suffix = interface.name[1:]  # 去掉 I 前缀
        iid_name = f"DAS_IID_{self._to_upper_snake(iid_suffix)}"

        # 根据是否在命名空间中选择不同的宏
        if namespace:
            # 在命名空间内，同时生成 DAS_IID_XXX 宏和前向声明
            return f"""// {{{interface.uuid}}}
DAS_DEFINE_GUID_IN_NAMESPACE(
    {iid_name},
    {namespace},
    {interface.name},
    {data1},
    {data2},
    {data3},
    {data4[0]},
    {data4[1]},
    {data4[2]},
    {data4[3]},
    {data4[4]},
    {data4[5]},
    {data4[6]},
    {data4[7]});
"""
        else:
            # 不在命名空间内，使用 DAS_DEFINE_GUID
            return f"""// {{{interface.uuid}}}
DAS_DEFINE_GUID(
    {iid_name},
    {interface.name},
    {data1},
    {data2},
    {data3},
    {data4[0]},
    {data4[1]},
    {data4[2]},
    {data4[3]},
    {data4[4]},
    {data4[5]},
    {data4[6]},
    {data4[7]});
"""

    def _generate_enum(self, enum: EnumDef) -> str:
        """生成枚举定义（使用普通 enum 以兼容老代码）"""
        lines = [f"enum {enum.name}", "{"]

        # 生成枚举值
        for i, value in enumerate(enum.values):
            suffix = "," if i < len(enum.values) - 1 else ""
            if value.value is not None:
                lines.append(f"{self.indent}{value.name} = {value.value}{suffix}")
            else:
                lines.append(f"{self.indent}{value.name}{suffix}")

        # 检查是否所有枚举值中有任何一个等于 0x7FFFFFFF
        # 0x7FFFFFFF 的十进制值是 2147483647
        FORCE_DWORD_VALUE = 0x7FFFFFFF  # 2147483647
        has_force_dword_value = False

        if enum.values:
            # 遍历所有枚举值，检查是否有任何一个等于 0x7FFFFFFF
            for value in enum.values:
                if value.value is not None:
                    # 尝试将值转换为整数进行比较
                    try:
                        if isinstance(value.value, str):
                            if value.value.startswith("0x"):
                                value_int = int(value.value, 16)
                            else:
                                value_int = int(value.value)
                        else:
                            # 已经是整数类型
                            value_int = int(value.value)

                        if value_int == FORCE_DWORD_VALUE:
                            has_force_dword_value = True
                            break
                    except ValueError:
                        pass

            # 如果没有任何枚举值等于 0x7FFFFFFF，则添加 FORCE_DWORD
            if not has_force_dword_value:
                import warnings
                force_dword_name = f"{self._to_upper_snake(enum.name)}_FORCE_DWORD"
                warnings.warn(
                    f"Enum '{enum.name}' does not contain a value equal to 0x7FFFFFFF. "
                    f"Adding {force_dword_name} = 0x7FFFFFFF",
                    UserWarning
                )
                # 在前一个值后面添加逗号
                lines[-1] = lines[-1] + ","
                lines.append(f"{self.indent}{force_dword_name} = 0x7FFFFFFF")
        else:
            # 枚举没有任何值，补充 FORCE_DWORD
            import warnings
            force_dword_name = f"{self._to_upper_snake(enum.name)}_FORCE_DWORD"
            warnings.warn(
                f"Enum '{enum.name}' has no values. "
                f"Adding {force_dword_name} = 0x7FFFFFFF",
                UserWarning
            )
            lines.append(f"{self.indent}{force_dword_name} = 0x7FFFFFFF")

        lines.append("};")
        return "\n".join(lines) + "\n"

    def _generate_struct(self, struct: StructDef) -> str:
        """生成结构体定义"""
        lines = [f"struct {struct.name}", "{"]

        # 生成结构体字段
        for field in struct.fields:
            # 使用 TYPE_MAP 将类型映射到 C++ 类型
            cpp_type = CppTypeMapper.TYPE_MAP.get(field.type_name, field.type_name)
            lines.append(f"{self.indent}{cpp_type} {field.name};")

        lines.append("};")
        return "\n".join(lines) + "\n"

    def _generate_method_signature(self, method: MethodDef, current_namespace: str = "") -> str:
        """生成方法签名

        Args:
            method: 方法定义
            current_namespace: 当前所在的命名空间
        """
        # 检查是否为 [binary_buffer] 方法
        is_binary_buffer = method.attributes.get('binary_buffer', False)

        # 参数列表
        params = []
        for param in method.parameters:
            if param.direction == ParamDirection.OUT:
                # 对于 [binary_buffer] 方法，保持原始类型（不添加额外的指针）
                if is_binary_buffer:
                    param_type = CppTypeMapper.map_type(param.type_info)
                else:
                    param_type = CppTypeMapper.get_out_param_type(param.type_info)
            else:
                param_type = CppTypeMapper.map_type(param.type_info)

            # 对接口类型应用命名空间限定
            base_type = param.type_info.base_type
            if CppTypeMapper.is_interface_type(base_type):
                qualified_type = self._qualify_type_if_needed(base_type, current_namespace)
                # 替换类型名中的基本类型为完全限定类型
                param_type = param_type.replace(base_type, qualified_type)

            params.append(f"{param_type} {param.name}")

        param_str = ", ".join(params) if params else ""

        # 返回类型
        return_type = CppTypeMapper.map_type(method.return_type)

        # 使用 DAS_METHOD 宏
        # bool类型也强制使用DasResult返回
        if return_type == "DasResult" or method.return_type.base_type == "bool":
            return f"DAS_METHOD {method.name}({param_str})"
        else:
            return f"DAS_METHOD_({return_type}) {method.name}({param_str})"

    def _generate_property_methods(self, prop: PropertyDef, current_namespace: str = "") -> List[str]:
        """根据属性定义生成 getter/setter 方法

        Args:
            prop: 属性定义
            current_namespace: 当前所在的命名空间
        """
        methods = []
        base_type = prop.type_info.base_type
        cpp_type = CppTypeMapper.TYPE_MAP.get(base_type, base_type)

        # 对接口类型应用命名空间限定
        if CppTypeMapper.is_interface_type(base_type):
            cpp_type = self._qualify_type_if_needed(base_type, current_namespace)

        is_interface = CppTypeMapper.is_interface_type(base_type)
        is_string = CppTypeMapper.is_string_type(base_type)

        if prop.has_getter:
            # Getter
            if is_interface or is_string:
                # 接口和字符串类型：DasResult GetXxx(IXxx** pp_out)
                out_type = f"{cpp_type}**" if is_interface else "IDasReadOnlyString**"
                methods.append(f"DAS_METHOD Get{prop.name}({out_type} pp_out)")
            else:
                # 基本类型：DasResult GetXxx(type* p_out)
                methods.append(f"DAS_METHOD Get{prop.name}({cpp_type}* p_out)")

        if prop.has_setter:
            # Setter
            if is_interface:
                # 接口类型：DasResult SetXxx(IXxx* p_value)
                methods.append(f"DAS_METHOD Set{prop.name}({cpp_type}* p_value)")
            elif is_string:
                # 字符串类型：DasResult SetXxx(IDasReadOnlyString* p_value)
                methods.append(f"DAS_METHOD Set{prop.name}(IDasReadOnlyString* p_value)")
            else:
                # 基本类型：DasResult SetXxx(type value)
                methods.append(f"DAS_METHOD Set{prop.name}({cpp_type} value)")

        return methods

    def _generate_interface(self, interface: InterfaceDef, namespace: str = "") -> str:
        """生成完整的接口定义（包含 GUID，用于全局命名空间）"""
        lines = []

        # UUID 定义
        uuid_def = self._generate_uuid_define(interface, namespace)
        if uuid_def:
            lines.append(uuid_def)

        # 接口体
        lines.append(self._generate_interface_body(interface, namespace))

        return "\n".join(lines) + "\n"

    def _generate_interface_body(self, interface: InterfaceDef, namespace: str = "") -> str:
        """生成接口体定义（不包含 UUID，用于命名空间内）"""
        lines = []

        # 接口声明
        lines.append(f"DAS_SWIG_EXPORT_ATTRIBUTE({interface.name})")
        lines.append(f"DAS_INTERFACE {interface.name} : public {interface.base_interface}")
        lines.append("{")

        # 方法
        for method in interface.methods:
            sig = self._generate_method_signature(method, namespace)
            lines.append(f"{self.indent}{sig} = 0;")

        # 属性生成的方法
        for prop in interface.properties:
            prop_methods = self._generate_property_methods(prop, namespace)
            for m in prop_methods:
                lines.append(f"{self.indent}{m} = 0;")

        lines.append("};")
        return "\n".join(lines) + "\n"

    def _get_import_includes(self) -> list:
        """根据 imports 生成 include 路径列表

        import语句不包含路径，只使用文件名。
        例如: import DasTypes; -> #include "DasTypes.h"
              import IDasGuidVector; -> #include "IDasGuidVector.h"
        """
        includes = []
        for imp in self.document.imports:
            # 从IDL文件路径中提取文件名（不含扩展名）
            idl_path = imp.idl_path
            # 获取文件名部分
            import_name = os.path.basename(idl_path)
            # 移除.idl扩展名
            if import_name.endswith('.idl'):
                import_name = import_name[:-4]
            # 生成对应的.h文件名
            h_path = import_name + '.h'
            includes.append(h_path)
        return includes

    def _collect_type_includes(self) -> set:
        """收集所有需要额外头文件的类型"""
        includes = set()

        # 遍历所有接口的方法和属性
        for interface in self.document.interfaces:
            # 检查基接口
            if CppTypeMapper.needs_dastypeinfo_include(interface.base_interface):
                includes.add('#include "IDasTypeInfo.h"')

            # 检查方法的返回类型和参数类型
            for method in interface.methods:
                # 检查返回类型
                if CppTypeMapper.needs_dasstring_include(method.return_type.base_type):
                    includes.add("#include <das/DasString.hpp>")
                if CppTypeMapper.needs_dastypeinfo_include(method.return_type.base_type):
                    includes.add('#include "IDasTypeInfo.h"')

                # 检查参数类型
                for param in method.parameters:
                    if CppTypeMapper.needs_dasstring_include(param.type_info.base_type):
                        includes.add("#include <das/DasString.hpp>")
                    if CppTypeMapper.needs_dastypeinfo_include(param.type_info.base_type):
                        includes.add('#include "IDasTypeInfo.h"')

            # 检查属性类型
            for prop in interface.properties:
                if CppTypeMapper.needs_dasstring_include(prop.type_info.base_type):
                    includes.add("#include <das/DasString.hpp>")
                if CppTypeMapper.needs_dastypeinfo_include(prop.type_info.base_type):
                    includes.add('#include "IDasTypeInfo.h"')

        return includes

    def generate_header(self, base_name: str) -> str:
        """生成 C++ 头文件"""
        guard_name = f"DAS_{base_name.upper()}_H"

        # 获取导入的 include 路径
        import_includes = self._get_import_includes()

        # 收集需要额外头文件的类型
        type_includes = self._collect_type_includes()

        # 按命名空间分组
        namespace_groups = {}
        no_namespace_structs = []
        no_namespace_enums = []
        no_namespace_interfaces = []

        for struct in self.document.structs:
            if struct.namespace:
                if struct.namespace not in namespace_groups:
                    namespace_groups[struct.namespace] = {'structs': [], 'enums': [], 'interfaces': []}
                namespace_groups[struct.namespace]['structs'].append(struct)
            else:
                no_namespace_structs.append(struct)

        for enum in self.document.enums:
            if enum.namespace:
                if enum.namespace not in namespace_groups:
                    namespace_groups[enum.namespace] = {'structs': [], 'enums': [], 'interfaces': []}
                namespace_groups[enum.namespace]['enums'].append(enum)
            else:
                no_namespace_enums.append(enum)

        for interface in self.document.interfaces:
            if interface.namespace:
                if interface.namespace not in namespace_groups:
                    namespace_groups[interface.namespace] = {'structs': [], 'enums': [], 'interfaces': []}
                namespace_groups[interface.namespace]['interfaces'].append(interface)
            else:
                no_namespace_interfaces.append(interface)

        # 收集所有前向声明（包括无命名空间和有命名空间的接口）
        forward_declarations = []

        # 无命名空间的前向声明
        if no_namespace_interfaces:
            for interface in no_namespace_interfaces:
                forward_declarations.append(f"DAS_INTERFACE {interface.name};")

        # 有命名空间的前向声明
        for namespace_name, items in sorted(namespace_groups.items()):
            if items['interfaces']:
                forward_declarations.append(f"namespace {namespace_name} {{")
                for interface in items['interfaces']:
                    forward_declarations.append(f"DAS_INTERFACE {interface.name};")
                forward_declarations.append(f"}} // namespace {namespace_name}")

        content = self._file_header(guard_name, import_includes, type_includes, forward_declarations)

        # 生成无命名空间的结构体
        for struct in no_namespace_structs:
            content += self._generate_struct(struct)
            content += "\n"

        # 生成无命名空间的枚举
        for enum in no_namespace_enums:
            content += self._generate_enum(enum)
            content += "\n"

        # 生成无命名空间的接口（不再生成前向声明，已在文件头生成）
        if no_namespace_interfaces:
            for interface in no_namespace_interfaces:
                content += self._generate_interface(interface)
                content += "\n"

        # 生成有命名空间的代码（支持 C++17 嵌套命名空间语法）
        for namespace_name, items in sorted(namespace_groups.items()):
            # 先在全局命名空间生成所有接口的 GUID 定义
            for interface in items['interfaces']:
                uuid_def = self._generate_uuid_define(interface, namespace_name)
                if uuid_def:
                    content += uuid_def
                    content += "\n"

            # 再在命名空间内生成结构体、枚举和接口声明
            content += self._generate_namespace_open(namespace_name)

            # 结构体
            for struct in items['structs']:
                content += self._generate_struct(struct)
                content += "\n"

            # 枚举
            for enum in items['enums']:
                content += self._generate_enum(enum)
                content += "\n"

            # 接口定义（不包含 GUID，因为已在全局命名空间定义）
            # 不再生成前向声明，已在文件头生成
            for interface in items['interfaces']:
                content += self._generate_interface_body(interface, namespace_name)
                content += "\n"

            content += self._generate_namespace_close(namespace_name)

        content += self._file_footer(guard_name)
        return content

    def get_interface_names(self) -> List[str]:
        """获取所有接口名称"""
        return [iface.name for iface in self.document.interfaces]

    def get_enum_names(self) -> List[str]:
        """获取所有枚举名称"""
        return [enum.name for enum in self.document.enums]


def generate_cpp_files(document: IdlDocument, output_dir: str, base_name: str, namespace: str = "", idl_file_path: str = None, global_type_map: dict = None) -> List[str]:
    """生成 C++ 文件

    Args:
        document: 已解析的 IDL 文档
        output_dir: 输出目录
        base_name: 基础文件名
        namespace: 命名空间
        idl_file_path: 原始 IDL 文件路径（用于解析相对导入，仅用于非批量模式）
        global_type_map: 全局类型到命名空间的映射（批量模式使用）
    """
    import os
    from das_idl_parser import parse_idl_file

    # 解析导入的 IDL 文件（仅用于非批量模式）
    imported_documents = {}

    # 如果提供了全局类型映射，直接使用它构建 imported_documents
    # 这样可以避免重复解析，并且支持批量处理
    if global_type_map:
        # 使用全局类型映射，不需要解析导入
        # 创建一个虚拟的 imported_documents，只包含类型信息
        pass  # global_type_map 将直接传递给生成器
    elif idl_file_path and document.imports:
        # 递归解析导入（仅用于非批量模式）
        idl_dir = os.path.dirname(os.path.abspath(idl_file_path))
        parsed_files = set()  # 防止循环导入

        def parse_imports_recursive(import_list, base_dir):
            for import_def in import_list:
                import_path = os.path.normpath(os.path.join(base_dir, import_def.idl_path))
                if import_path in parsed_files:
                    continue

                parsed_files.add(import_path)
                try:
                    if os.path.exists(import_path):
                        imported_doc = parse_idl_file(import_path)
                        import_filename = os.path.splitext(os.path.basename(import_def.idl_path))[0]
                        imported_documents[import_filename] = imported_doc

                        # 递归解析导入的文件的导入
                        if imported_doc.imports:
                            import_base_dir = os.path.dirname(import_path)
                            parse_imports_recursive(imported_doc.imports, import_base_dir)
                except Exception as e:
                    print(f"警告: 无法解析导入的 IDL 文件 {import_path}: {e}")

        parse_imports_recursive(document.imports, idl_dir)

    # 从IDL文件路径中提取文件名
    idl_file_name = None
    if idl_file_path:
        idl_file_name = os.path.basename(idl_file_path)

    generator = CppCodeGenerator(document, namespace, imported_documents, global_type_map, idl_file_name)

    # 确保输出目录存在
    os.makedirs(output_dir, exist_ok=True)

    # 生成头文件
    header_content = generator.generate_header(base_name)
    header_path = os.path.join(output_dir, f"{base_name}.h")
    with open(header_path, 'w', encoding='utf-8') as f:
        f.write(header_content)
    print(f"Generated: {header_path}")

    return [header_path]


# 测试代码
if __name__ == '__main__':
    from das_idl_parser import parse_idl

    test_idl = '''
    enum DasTestStatus {
        None = 0,
        Running = 1,
        Completed = 2,
        Failed = -1,
    }

    [uuid("D5B3213B-B7C4-4194-0E99-5CEFFF064F8D")]
    interface IDasGuidVector : IDasBase {
        DasResult Size([out] size_t* p_out_size);
        DasResult At(size_t index, [out] DasGuid* p_out_iid);
        DasResult Find(const DasGuid& iid);
        DasResult PushBack(const DasGuid& iid);
    }

    [uuid("a1234567-89ab-cdef-0123-456789abcdef")]
    interface IDasTestResult : IDasBase {
        [get, set] int32 Id
        [get] DasReadOnlyString Name
        [get, set] DasGuid Guid
    }
    '''

    doc = parse_idl(test_idl)
    generator = CppCodeGenerator(doc)

    print("=== 生成的 C++ 头文件 ===")
    print(generator.generate_header("IDasTest"))
