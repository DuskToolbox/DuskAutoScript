"""
DAS SWIG 代码生成器
根据解析后的 IDL 定义生成 SWIG .i 文件

生成内容:
1. 每个接口对应的 .i 文件，包含:
   - %ignore 指令 (隐藏 AddRef/Release)
   - %feature("director") 指令 (支持目标语言继承)
   - %typemap 指令 (将 [out] 参数转换为返回值，DasResult < 0 抛异常)
   - 辅助方法和类型定义

2. 汇总的 .i 文件，用于 include 所有生成的 .i 文件
"""
import json
import os
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import List, Optional, Set, Dict
from das_idl_parser import (
    IdlDocument, InterfaceDef, EnumDef, MethodDef, PropertyDef,
    ParameterDef, TypeInfo, ParamDirection, TypeKind,
)
from das_idl_parser import parse_idl_file as _das_idl_parser_parse_idl_file
from swig_java_generator import JavaSwigGenerator
from swig_csharp_generator import CSharpSwigGenerator
from swig_python_generator import PythonSwigGenerator
from swig_lua_generator import LuaSwigGenerator
from swig_lang_generator_base import SwigLangGenerator, SwigLangGeneratorContext
from swig_api_model import build_swig_interface_model, build_interface_map, SwigInterfaceModel
BaseSwigGenerator = SwigLangGenerator

# [binary_buffer] 方法允许的参数类型（只支持 unsigned char** 系列）
BINARY_BUFFER_ALLOWED_TYPES = {
    'unsigned char',   # unsigned char**
    'uint8_t',         # uint8_t** (等价于 unsigned char**)
}


class SwigTypeMapper:
    """SWIG 类型映射器"""

    # 基本类型到 SWIG 返回类型的映射
    SWIG_RET_TYPE_MAP = {
        'int32': 'DasRetInt',
        'int64': 'DasRetInt',
        'uint32': 'DasRetUInt',
        'uint64': 'DasRetUInt',
        'size_t': 'DasRetUInt',
        'float': 'DasRetFloat',
        'double': 'DasRetFloat',
        'bool': 'DasRetBool',
        'DasBool': 'DasRetBool',
        'DasGuid': 'DasRetGuid',
        'int': 'DasRetInt',
        'uint': 'DasRetUInt',
    }

    @staticmethod
    def is_string_type(type_name: str) -> bool:
        """判断是否是字符串类型"""
        return type_name in ('DasString', 'DasReadOnlyString', 'IDasReadOnlyString')

    @classmethod
    def get_swig_ret_type(cls, type_info: TypeInfo) -> str:
        """获取 SWIG 返回类型"""
        base = type_info.base_type

        if base in cls.SWIG_RET_TYPE_MAP:
            return cls.SWIG_RET_TYPE_MAP[base]

        # 接口类型返回 DasRetXxx
        if type_info.type_kind == TypeKind.INTERFACE:
            if base.startswith('IDas'):
                return f"DasRet{base[4:]}"
            else:
                return f"DasRet{base[1:]}"

        # 字符串类型
        if cls.is_string_type(base):
            return 'DasRetReadOnlyString'

        return 'DasResult'

    @classmethod
    def get_interface_ret_type_name(cls, interface_name: str) -> str:
        """获取接口的返回类型名称"""
        if interface_name.startswith('IDas'):
            return f"DasRet{interface_name[4:]}"
        else:
            return f"DasRet{interface_name[1:]}"


class SwigCodeGenerator:
    """SWIG .i 文件生成器"""

    _global_typemaps: dict[str, str] = {}
    _global_ret_classes: dict[str, str] = {}
    _global_header_blocks: dict[str, str] = {}
    _global_typemaps_ignore: dict[str, str] = {}

    def __init__(self, document: IdlDocument, idl_file_name: Optional[str] = None, idl_file_path: Optional[str] = None, lang_generators: Optional[List[SwigLangGenerator]] = None, debug: bool = False):
        self.document = document
        self.idl_file_name = idl_file_name
        self.idl_file_path = idl_file_path
        self.debug = debug
        self.indent = "    "
        self._collected_interface_types: Set[str] = set()
        self._current_typemaps: List[str] = []
        self._imported_documents: Dict[str, IdlDocument] = {}
        self._parse_imported_documents()

        if lang_generators is None:
            self.lang_generators = [JavaSwigGenerator(), CSharpSwigGenerator(), PythonSwigGenerator(), LuaSwigGenerator()]
        else:
            self.lang_generators = lang_generators

        # 为所有语言生成器设置上下文
        self._setup_lang_generator_contexts()

    def _setup_lang_generator_contexts(self) -> None:
        """为所有语言生成器设置上下文"""
        context = SwigLangGeneratorContext(
            get_interface_namespace_func=self._get_type_namespace,
            get_interface_idl_file_func=self._get_interface_idl_file_name,
            get_enum_idl_file_func=self._get_enum_idl_file_name,
            global_ret_classes=self._global_ret_classes,
            global_typemaps=self._global_typemaps,
            global_header_blocks=self._global_header_blocks,
            global_typemaps_ignore=self._global_typemaps_ignore
        )
        for lang_generator in self.lang_generators:
            lang_generator.set_context(context)
            lang_generator.debug = self.debug

    def _parse_imported_documents(self) -> None:
        """解析导入的 IDL 文件，构建导入文档映射"""
        if not self.idl_file_path or not self.document.imports:
            return

        # 获取当前 IDL 文件所在的目录（相对于 idl 根目录）
        idl_path = Path(self.idl_file_path)
        parts_lower = [p.lower() for p in idl_path.parts]
        if 'idl' not in parts_lower:
            return

        idx = parts_lower.index('idl')
        idl_dir = Path(*idl_path.parts[:idx + 1])
        current_idl_dir = idl_path.parent if idl_path != idl_dir else idl_dir

        # 递归解析导入的 IDL 文件
        parsed_files = set()

        def parse_imports_recursive(import_list, base_dir):
            for import_def in import_list:
                import_path_str = import_def.idl_path.strip('"')
                import_path = Path(base_dir) / import_path_str

                # 规范化路径
                try:
                    import_path = import_path.resolve()
                except:
                    continue

                # 检查是否已解析
                import_key = str(import_path)
                if import_key in parsed_files:
                    continue

                parsed_files.add(import_key)

                # 解析 IDL 文件
                try:
                    if import_path.exists():
                        imported_doc = _das_idl_parser_parse_idl_file(str(import_path))
                        # 使用文件名（不含扩展名）作为键
                        import_filename = import_path.stem
                        self._imported_documents[import_filename] = imported_doc

                        # 递归解析导入的文件的导入
                        if imported_doc.imports:
                            import_base_dir = import_path.parent
                            parse_imports_recursive(imported_doc.imports, import_base_dir)
                except Exception as e:
                    print(f"警告: 无法解析导入的 IDL 文件 {import_path}: {e}")

        parse_imports_recursive(self.document.imports, current_idl_dir)

    def _is_binary_buffer_interface(self, interface: InterfaceDef) -> bool:
        """判断是否是二进制缓冲区接口"""
        return interface.name.endswith('BinaryBuffer')



    def _get_interface_namespace(self, interface_name: str) -> str | None:
        """根据接口名查找其命名空间

        Args:
            interface_name: 接口名称（可以是简单名或完全限定名）

        Returns:
            命名空间字符串，如果未找到则返回 None
        """
        # 提取简单名称
        simple_name = interface_name.split('::')[-1]

        # 先在当前文档中查找接口
        for interface in self.document.interfaces:
            if interface.name == simple_name:
                return interface.namespace

        # 在所有导入的文档中查找接口
        for doc in self._imported_documents.values():
            for interface in doc.interfaces:
                if interface.name == simple_name:
                    return interface.namespace
        return None

    def _get_type_namespace(self, type_name: str) -> str | None:
        """根据类型名查找其命名空间（支持接口、结构体、枚举）

        Args:
            type_name: 类型名称（可以是简单名或完全限定名）

        Returns:
            命名空间字符串，如果未找到则返回 None
        """
        # 检查 type_name 是否已经包含命名空间（完全限定名）
        if '::' in type_name:
            # 提取最后一个命名空间部分
            parts = type_name.split('::')
            simple_name = parts[-1]
            # 命名空间是除了最后一个部分之外的所有部分
            namespace = '::'.join(parts[:-1])
            # 验证简单名称是否存在于类型定义中
            # 先在当前文档中查找
            for interface in self.document.interfaces:
                if interface.name == simple_name and interface.namespace == namespace:
                    return namespace
            for struct in self.document.structs:
                if struct.name == simple_name and struct.namespace == namespace:
                    return namespace
            for enum in self.document.enums:
                if enum.name == simple_name and enum.namespace == namespace:
                    return namespace
            # 在所有导入的文档中查找
            for doc in self._imported_documents.values():
                for interface in doc.interfaces:
                    if interface.name == simple_name and interface.namespace == namespace:
                        return namespace
                for struct in doc.structs:
                    if struct.name == simple_name and struct.namespace == namespace:
                        return namespace
                for enum in doc.enums:
                    if enum.name == simple_name and enum.namespace == namespace:
                        return namespace
            # 如果找不到完全匹配的类型，返回 None
            return None
        else:
            # 提取简单名称
            simple_name = type_name

            # 先在当前文档中查找
            # 查找接口
            for interface in self.document.interfaces:
                if interface.name == simple_name:
                    return interface.namespace

            # 查找结构体
            for struct in self.document.structs:
                if struct.name == simple_name:
                    return struct.namespace

            # 查找枚举
            for enum in self.document.enums:
                if enum.name == simple_name:
                    return enum.namespace

            # 在所有导入的文档中查找
            for doc in self._imported_documents.values():
                # 查找接口
                for interface in doc.interfaces:
                    if interface.name == simple_name:
                        return interface.namespace

                # 查找结构体
                for struct in doc.structs:
                    if struct.name == simple_name:
                        return struct.namespace

                # 查找枚举
                for enum in doc.enums:
                    if enum.name == simple_name:
                        return enum.namespace

            return None

    def _get_interface_idl_file_name(self, type_name: str) -> str | None:
        """根据类型名查找其所在的 IDL 文件名（不含扩展名）
        
        Args:
            type_name: 类型名称（可以是简单名或完全限定名）
            
        Returns:
            IDL 文件名（不含扩展名），如果未找到则返回 None
        """
        simple_name = type_name.split('::')[-1]
        
        # 先在当前文档中查找
        for interface in self.document.interfaces:
            if interface.name == simple_name:
                if self.idl_file_name:
                    return Path(self.idl_file_name).stem
                return None
        
        # 在所有导入的文档中查找
        for doc_path, doc in self._imported_documents.items():
            for interface in doc.interfaces:
                if interface.name == simple_name:
                    return Path(doc_path).stem
        
        return None

    def _get_enum_idl_file_name(self, type_name: str) -> str | None:
        """根据枚举类型名查找其所在的 IDL 文件名
        
        Args:
            type_name: 枚举类型名称
            
        Returns:
            IDL 文件名（不含扩展名），如果未找到则返回 None
        """
        simple_name = type_name.split('::')[-1]
        
        # 在当前文档中查找枚举
        for enum in self.document.enums:
            if enum.name == simple_name:
                if self.idl_file_name:
                    return Path(self.idl_file_name).stem
                return None
        
        # 在所有导入的文档中查找枚举
        for doc_path, doc in self._imported_documents.items():
            for enum in doc.enums:
                if enum.name == simple_name:
                    return Path(doc_path).stem
        
        return None

    def _is_binary_data_method(self, interface: InterfaceDef, method: MethodDef) -> bool:
        """判断是否是返回二进制数据的方法

        [binary_buffer] 方法必须满足：
        1. 有 [binary_buffer] 属性标记
        2. 输出参数类型为 unsigned char** 或 const unsigned char**
        """
        return method.attributes.get('binary_buffer', False) if method.attributes else False

    def _validate_binary_buffer_method(self, interface: InterfaceDef, method: MethodDef) -> bool:
        """验证 [binary_buffer] 方法的参数类型是否合法

        [binary_buffer] 方法的输出参数必须是 unsigned char** 或 uint8_t**
        返回 True 表示验证通过，False 表示验证失败
        """
        if not method.attributes.get('binary_buffer', False):
            return True  # 非 binary_buffer 方法，无需验证

        # 查找 [out] 参数
        for param in method.parameters:
            if param.direction == ParamDirection.OUT:
                base_type = param.type_info.base_type
                # 检查基础类型是否为允许的类型
                if base_type not in BINARY_BUFFER_ALLOWED_TYPES:
                    print(f"Warning: [binary_buffer] method {interface.name}::{method.name} "
                          f"has invalid parameter type '{base_type}'. "
                          f"Expected: unsigned char* or uint8_t*")
                    return False

        return True

    def _file_header(self, module_name: str) -> str:
        """生成 .i 文件头"""
        timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

        # 生成IDL文件名注释
        idl_file_comment = ""
        if self.idl_file_name:
            idl_file_comment = f"// Source IDL file: {self.idl_file_name}\n"

        return f"""// This file is automatically generated by DAS IDL Generator
// Generated at: {timestamp}
{idl_file_comment}// !!! DO NOT EDIT !!!

// SWIG interface file for {module_name}

"""

    def _to_upper_snake(self, name: str) -> str:
        """将 PascalCase 转换为 UPPER_SNAKE_CASE"""
        result = []
        for i, c in enumerate(name):
            if c.isupper() and i > 0:
                result.append('_')
            result.append(c.upper())
        return ''.join(result)

    def _get_qualified_name(self, name: str, namespace: str) -> str:
        """获取带命名空间的限定名称"""
        if namespace:
            return f"{namespace}::{name}"
        return name

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

    def _collect_interface_types(self, interface: InterfaceDef):
        """收集接口中使用的所有接口类型"""
        for method in interface.methods:
            # 检查参数中的接口类型
            for param in method.parameters:
                if param.direction == ParamDirection.OUT:
                    if param.type_info.type_kind == TypeKind.INTERFACE:
                        self._collected_interface_types.add(param.type_info.base_type)

        for prop in interface.properties:
            if prop.type_info.type_kind == TypeKind.INTERFACE:
                self._collected_interface_types.add(prop.type_info.base_type)

    def _generate_ignore_directives(self, interface: InterfaceDef) -> str:
        """生成 %ignore 指令 - 隐藏引用计数方法"""
        qualified_name = self._get_qualified_name(interface.name, interface.namespace)
        lines = []
        lines.append(f"// Hide reference counting methods from target language")
        lines.append(f"%ignore {qualified_name}::AddRef;")
        lines.append(f"%ignore {qualified_name}::Release;")
        lines.append(f"%ignore {qualified_name}::QueryInterface;")
        return "\n".join(lines) + "\n"

    def _generate_director_directive(self, interface: InterfaceDef) -> str:
        """生成 %feature("director") 指令 - 支持目标语言继承"""
        qualified_name = self._get_qualified_name(interface.name, interface.namespace)
        return f"""// Enable director for inheritance in target language
%feature("director") {qualified_name};
"""


    def _generate_lang_specific_out_param_wrappers(self, interface: InterfaceDef) -> str:
        """生成语言特定的 [out] 参数包装代码（包含 DasRetXxx 类型定义）

        此方法应该在 %include 指令之后调用，以确保 SWIG 已经看到所有类型定义
        """
        lang_codes = []
        
        # 处理单 [out] 参数的方法
        for method in interface.methods:
            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            if len(out_params) == 1:
                if self._is_binary_data_method(interface, method):
                    continue
                # 检查是否是多返回值方法（Java专用）
                is_multi_out = False
                for lang_generator in self.lang_generators:
                    if lang_generator.__class__.__name__ == 'JavaSwigGenerator':
                        if hasattr(lang_generator, '_get_multi_out_param_methods'):
                            multi_out_methods = lang_generator._get_multi_out_param_methods(interface)
                            if any(m.name == method.name for m, _ in multi_out_methods):
                                is_multi_out = True
                                break
                if is_multi_out:
                    continue
                for lang_generator in self.lang_generators:
                    lang_code = lang_generator.generate_out_param_wrapper(interface, method, out_params[0])
                    if lang_code:
                        lang_codes.append(lang_code)
        
        # 处理多 [out] 参数的方法（Java 专用）
        # 注意：多返回值类型定义和包装代码都通过 _global_typemaps 写入 DasTypeMapsExtend.i
        # 不要直接添加到 lang_codes，避免重复定义（Warning 302）
        for lang_generator in self.lang_generators:
            if lang_generator.__class__.__name__ == 'JavaSwigGenerator':
                if hasattr(lang_generator, '_get_multi_out_param_methods'):
                    multi_out_methods = lang_generator._get_multi_out_param_methods(interface)
                    for method, out_params in multi_out_methods:
                        # 生成多返回值类型定义（通过 _global_ret_classes 存储）
                        if hasattr(lang_generator, '_generate_multi_ret_class'):
                            lang_generator._generate_multi_ret_class(out_params, interface.name)
                        # 生成多返回值包装代码（通过 _global_typemaps 存储）
                        if hasattr(lang_generator, '_generate_multi_out_wrapper'):
                            lang_generator._generate_multi_out_wrapper(interface, method, out_params)
        
        return "\n".join(lang_codes)

    def _get_cpp_type(self, type_name: str) -> str:
        """获取 C++ 类型"""
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
        }
        return TYPE_MAP.get(type_name, type_name)

    def _get_swig_from_type(self, type_name: str) -> str:
        """获取 SWIG_From_xxx 函数的类型后缀"""
        SWIG_FROM_MAP = {
            'bool': 'bool',
            'int8': 'int',
            'int16': 'int',
            'int32': 'int',
            'int64': 'long_SS_long',
            'uint8': 'unsigned_SS_int',
            'uint16': 'unsigned_SS_int',
            'uint32': 'unsigned_SS_int',
            'uint64': 'unsigned_SS_long_SS_long',
            'float': 'float',
            'double': 'double',
            'size_t': 'size_t',
            'int': 'int',
            'uint': 'unsigned_SS_int',
        }
        return SWIG_FROM_MAP.get(type_name, 'int')

    def _generate_exception_check(self, interface: InterfaceDef) -> str:
        """生成 DasResult 异常检查"""
        return f"""
// Exception check for DasResult return values
%typemap(out) DasResult {{
    if ($1 < 0) {{
        SWIG_exception_fail(SWIG_RuntimeError, "DasResult error");
    }}
    $result = SWIG_From_int($1);
}}
"""

    def _generate_cross_namespace_wrapper_usings(self, interface: InterfaceDef) -> str:
        """为跨命名空间类型生成 wrapper 代码中的 using 声明

        当 ABI header 使用 #ifdef SWIG 让 SWIG 看到不带限定的类型名时，
        SWIG 会把这些类型解析到当前命名空间（如 Das::ExportInterface::IDasCapture），
        导致生成的 wrapper 代码中出现错误的命名空间限定。

        通过在 wrapper 代码中注入 using 声明：
          namespace Das::ExportInterface { using Das::PluginInterface::IDasCapture; }
        让 C++ 编译器能正确解析这些类型。
        """
        if not interface.namespace:
            return ''

        usings: list[str] = []
        seen: set[str] = set()

        for method in interface.methods:
            for param in method.parameters:
                base_type = param.type_info.base_type
                if param.type_info.type_kind != TypeKind.INTERFACE:
                    continue
                if SwigTypeMapper.is_string_type(base_type):
                    continue
                if base_type in seen:
                    continue

                # 查找类型的命名空间
                type_ns = ''
                for iface in self._all_documents_interfaces():
                    if iface.name == base_type:
                        type_ns = iface.namespace
                        break

                # 只处理跨有名命名空间的类型
                if type_ns and type_ns != interface.namespace:
                    usings.append(f"using {type_ns}::{base_type};")
                    seen.add(base_type)

        if not usings:
            return ''

        lines = [
            '',
            '// Inject using declarations into wrapper code for cross-namespace types.',
            '// SWIG resolves unqualified types to the current namespace,',
            '// so C++ needs using to find the actual type.',
            '%{',
            f'namespace {interface.namespace} {{',
        ]
        lines.extend(usings)
        lines.append(f'}} // namespace {interface.namespace}')
        lines.append('%}')

        return '\n'.join(lines)

    def _generate_binary_buffer_typemaps(self, interface: InterfaceDef) -> str:
        """为二进制缓冲区接口生成特殊的 typemap

        为 Python/C#/Java 生成目标语言友好的二进制数据访问方法
        [binary_buffer] 方法的参数类型限定为 unsigned char** 或 const unsigned char**
        """
        if not self._is_binary_buffer_interface(interface):
            return ""

        for method in interface.methods:
            self._validate_binary_buffer_method(interface, method)

        binary_buffer_method_name = None
        for method in interface.methods:
            if method.attributes.get('binary_buffer', False):
                binary_buffer_method_name = method.name
                break

        if not binary_buffer_method_name:
            return ""

        if interface.name == "IDasImage":
            size_method_name = "GetDataSize"
        else:
            size_method_name = "GetSize"

        qualified_name = self._get_qualified_name(interface.name, interface.namespace)
        lines = []

        lines.append(f"""
// ============================================================================
// Binary Buffer Support for {interface.name}
// 为二进制数据提供目标语言友好的访问方式
// [binary_buffer] 方法参数类型: unsigned char** (统一类型，简化处理)
// ============================================================================
""")

        lang_codes = []
        for lang_generator in self.lang_generators:
            lang_code = lang_generator.generate_binary_buffer_helpers(interface, binary_buffer_method_name, size_method_name)
            if lang_code:
                lang_codes.append(lang_code)
                typemap_pattern = re.compile(r'%typemap\s*\(([^)]+)\)\s*(\S+)\s*\{')
                for match in typemap_pattern.finditer(lang_code):
                    typemap_sig = f"{match.group(2)} ({match.group(1)})"
                    self._global_typemaps[typemap_sig] = lang_code

        lines.append("\n".join(lang_codes))
        return "\n".join(lines)


    def _generate_director_out_param_typemaps(self, interface: InterfaceDef) -> str:
        """为 Director 类生成所有 out 参数的 typemap
        
        Director 类需要实现原始方法签名，但 Java 端需要将 out 参数转换为返回值。
        此方法生成所有带有单个 [out] 参数的方法的 javadirectorout typemap。
        
        Args:
            interface: 接口定义
            
        Returns:
            javadirectorout typemap 代码字符串
        """
        typemaps = []
        
        for method in interface.methods:
            # 查找带有单个 [out] 参数的方法
            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            
            # 只处理有且仅有一个 [out] 参数的方法
            if len(out_params) == 1:
                out_param = out_params[0]
                
                # 为每个语言生成器调用 _generate_director_out_param_typemap
                for lang_generator in self.lang_generators:
                    if hasattr(lang_generator, '_generate_director_out_param_typemap'):
                        typemap = lang_generator._generate_director_out_param_typemap(
                            interface, method, out_param
                        )
                        if typemap:
                            typemaps.append(typemap)
        
        return "\n".join(typemaps)

    def _generate_clear_typemaps(self) -> str:
        """检查是否有typemap需要清除

        不再在文件末尾生成%clear指令，因为typemap将统一在DasTypeMaps.i中定义。
        """
        return ""

    @classmethod
    def get_global_typemaps(cls) -> dict:
        """获取全局typemap字典"""
        return cls._global_typemaps

    @classmethod
    def get_global_ret_classes(cls) -> dict:
        """获取全局ret_classes字典"""
        return cls._global_ret_classes

    def _generate_iid_static_method(self, interface: InterfaceDef) -> str:
        """为接口生成 %extend IID() 静态方法

        在 C++ 层注入静态函数，返回 DasIidOf<Interface> 的值。
        这样 Java 端可以通过 Interface.IID() 获取接口的 IID，无需反射。
        """
        qualified_name = self._get_qualified_name(interface.name, interface.namespace)

        return f"""
// ============================================================================
// Static IID method for {interface.name}
// ============================================================================
%extend {qualified_name} {{
    static DasGuid IID() {{
        return DasIidOf<{qualified_name}>();
    }}
}}
"""

    def _get_swig_interface_name(self, interface_name: str) -> str:
        """获取导出给 SWIG 的接口名称 (IDasXxx -> ISwigDasXxx)"""
        if interface_name.startswith('I'):
            return f"ISwig{interface_name[1:]}"
        else:
            return f"ISwig{interface_name}"

    def _find_interface_by_name(self, interface_name: str, namespace: str = "") -> Optional[InterfaceDef]:
        """在文档中查找指定名称的接口（包括导入的文档）"""
        # 首先在当前文档中查找
        for iface in self.document.interfaces:
            if iface.name == interface_name:
                # 如果指定了命名空间，必须匹配命名空间
                if namespace:
                    return iface if iface.namespace == namespace else None
                # 如果没指定命名空间，返回第一个匹配的
                return iface

        # 然后在导入的文档中查找
        for doc in self._imported_documents.values():
            for iface in doc.interfaces:
                if iface.name == interface_name:
                    # 如果指定了命名空间，必须匹配命名空间
                    if namespace:
                        return iface if iface.namespace == namespace else None
                    # 如果没指定命名空间，返回第一个匹配的
                    return iface

        return None



    def _get_ret_class_name(self, out_type: str) -> str:
        """根据 [out] 参数类型生成返回包装类名（DasRetXxx）

        复用 JavaSwigGenerator 中的逻辑。
        """
        # 委托给 JavaSwigGenerator 的静态方法
        return JavaSwigGenerator._get_ret_class_name(out_type)

    def _generate_director_bridge_methods(self, interface: InterfaceDef) -> str:
        """为 ISwig 类生成 director 桥接方法

        对于每个带 [out] 参数的方法，生成两个方法：
        1. virtual DasRetXxx MethodName(non-out-params) — 可通过 director 被 Java 覆盖
        2. DasResult MethodName(all-params) override final — 转发到 DasRetXxx 版本

        这样 Java 子类可以覆盖返回 DasRetXxx 的方法，
        而 C++ 调用原始虚方法时会自动通过 director 桥接到 Java。
        """
        lines = []

        for method in interface.methods:
            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            if len(out_params) != 1:
                continue

            # 跳过 [binary_buffer] 方法 — 这些方法由 generate_binary_buffer_helpers 处理
            if method.attributes and method.attributes.get('binary_buffer', False):
                continue

            out_param = out_params[0]
            out_type = out_param.type_info.base_type

            # 跳过字符串类型的 out 参数 — 已通过 DasReadOnlyString.i 的 director typemap 处理
            if SwigTypeMapper.is_string_type(out_type):
                continue

            if not self._should_generate_bridge(method):
                continue

            ret_class_name = self._get_ret_class_name(out_type)

            # 收集非 [out] 参数
            in_params = [p for p in method.parameters if p.direction != ParamDirection.OUT]

            # 构建参数列表
            cpp_in_params = []  # DasRetXxx 方法的参数列表
            cpp_all_params = []  # 原始方法的完整参数列表
            call_args = []  # 调用基类方法时的参数
            in_call_args = []  # 调用 DasRetXxx 方法时的参数

            for param in method.parameters:
                cpp_type = self._get_cpp_type(param.type_info.base_type)

                if param.direction == ParamDirection.OUT:
                    # [out] 参数：接口类型固定 **, 其他类型 *
                    # 使用无限定类型（SWIG 通过 using 声明解析为当前命名空间类型）
                    # 这样 SWIG 能识别 3-param override 覆写了基类纯虚方法
                    if param.type_info.type_kind == TypeKind.INTERFACE:
                        ptr_type = f'{cpp_type}**'
                    else:
                        ptr_type = f'{cpp_type}{"*" * param.type_info.pointer_level}'
                    cpp_all_params.append(f'{ptr_type} {param.name}')
                    call_args.append(f'&result.value')
                else:
                    # [in] 参数 — 使用无限定类型
                    # SWIG 通过 using 声明（在基类头文件中）解析为当前命名空间限定名
                    # 这使 SWIG 能识别 %inline 块中的方法签名与基类方法一致
                    if param.type_info.is_const and param.type_info.is_reference:
                        param_decl = f'const {cpp_type}& {param.name}'
                    elif param.type_info.is_pointer:
                        stars = '*' * param.type_info.pointer_level
                        const_prefix = 'const ' if param.type_info.is_const else ''
                        param_decl = f'{const_prefix}{cpp_type}{stars} {param.name}'
                    elif param.type_info.is_reference:
                        param_decl = f'{cpp_type}& {param.name}'
                    else:
                        param_decl = f'{cpp_type} {param.name}'

                    cpp_in_params.append(param_decl)
                    cpp_all_params.append(param_decl)
                    call_args.append(param.name)
                    in_call_args.append(param.name)

            in_params_str = ', '.join(cpp_in_params)
            all_params_str = ', '.join(cpp_all_params)
            call_args_str = ', '.join(call_args)
            in_call_args_str = ', '.join(in_call_args)

            # 初始化 out param 的默认值
            out_cpp_type = self._get_cpp_type(out_type)
            if out_param.type_info.type_kind == TypeKind.INTERFACE:
                default_init = 'nullptr'
            elif out_type == 'bool':
                default_init = 'false'
            elif self._is_struct_type(out_type):
                # Struct types (DasGuid, DasSize, etc.) — aggregate init
                default_init = '{{}}'
            else:
                # Scalar types: enums, numeric integers, float/double
                # Use static_cast to avoid implicit conversion warnings with -Wall
                default_init = f'static_cast<{out_cpp_type}>(0)'

            lines.append(f'')
            lines.append(f'    // Director bridge: {method.name} — DasRetXxx virtual method')
            lines.append(f'    virtual {ret_class_name} {method.name}({in_params_str}) {{')
            lines.append(f'        {ret_class_name} result;')
            lines.append(f'        result.value = {default_init};')
            lines.append(f'        result.error_code = DAS_E_NO_IMPLEMENTATION;')
            lines.append(f'        return result;')
            lines.append(f'    }}')
            lines.append(f'')
            lines.append(f'    // Raw override: delegates to DasRetXxx virtual method above')
            lines.append(f'    DasResult {method.name}({all_params_str}) override {{')
            lines.append(f'        auto result = this->{method.name}({in_call_args_str});')
            lines.append(f'        if ({out_param.name}) *{out_param.name} = result.value;')
            lines.append(f'        return result.error_code;')
            lines.append(f'    }}')

        return '\n'.join(lines)

    def _should_generate_bridge(self, method: MethodDef) -> bool:
        """判断方法是否需要生成 director 桥接

        过滤条件（在调用前已检查 out_params == 1、非 binary_buffer、非 string）：
        - 这里可以添加额外的过滤逻辑
        """
        return True

    def _qualify_param_type(self, cpp_type: str, base_type: str, type_kind: TypeKind, interface: InterfaceDef) -> str:
        """为接口/枚举类型添加命名空间限定

        在 ISwig 类的 %inline 块中，类定义在特定命名空间内（如 Das::PluginInterface），
        但参数类型可能在另一个命名空间（如 Das::ExportInterface）。
        需要添加 :: 前缀或完全限定名，避免 C++ 编译器找不到类型。
        """
        if type_kind != TypeKind.INTERFACE:
            return cpp_type

        # 查找类型所在的命名空间
        type_ns = ''
        for iface in self._all_documents_interfaces():
            if iface.name == base_type:
                type_ns = iface.namespace
                break

        if not type_ns:
            # 全局命名空间，添加 :: 前缀
            return f'::{cpp_type}'
        elif type_ns != interface.namespace:
            # 不同命名空间，使用完全限定名
            return f'::{type_ns}::{cpp_type}'
        else:
            # 同一命名空间，无需限定
            return cpp_type

    def _get_bridged_method_names(self, interface: InterfaceDef) -> list[str]:
        """获取需要生成 director 桥接的方法名列表"""
        names = []
        for method in interface.methods:
            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            if len(out_params) != 1:
                continue
            if method.attributes and method.attributes.get('binary_buffer', False):
                continue
            out_type = out_params[0].type_info.base_type
            if SwigTypeMapper.is_string_type(out_type):
                continue
            if not self._should_generate_bridge(method):
                continue
            names.append(method.name)
        return names

    def _generate_nodirector_for_raw_methods(self, interface: InterfaceDef, swig_name: str) -> str:
        """为桥接方法的原始 override 版本生成 %feature("nodirector") 指令

        已禁用。现在 %inline 块中的 3-param override 使用无限定类型，
        SWIG 能识别它覆写了基类纯虚方法，因此不会为该方法生成 Director。
        基类的 %feature("nodirector") 已足够阻止 Director 生成。
        """
        return ''

    def _generate_nodirector_for_base_class(self, interface: InterfaceDef, clear: bool = False) -> str:
        """为基类中需要桥接的方法生成 %feature("nodirector") 指令

        这些指令必须在 %include 基类头文件之前生成，这样 SWIG 解析基类时
        就能标记 feature，后续构建 Director vtable 时不会为基类纯虚方法
        生成 throw-pure-virtual 空壳。

        签名必须匹配 SWIG 解析基类头文件后看到的类型：
        - 全局命名空间类型（如 IDasBase）→ 使用 ::IDasBase（全局限定前缀）
        - 其他命名空间类型（通过 using 引入）→ 使用基类命名空间限定名
        - 同命名空间类型 → 保持无限定

        当 clear=True 时，生成 %feature("nodirector", "0") 来清除之前设置的
        nodirector feature。必须在 %inline 块之后调用，防止全局 feature 污染
        其他 .i 文件中继承同一基类的接口。
        """
        lines = []

        for method in interface.methods:
            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            if len(out_params) != 1:
                continue
            if method.attributes and method.attributes.get('binary_buffer', False):
                continue
            out_type = out_params[0].type_info.base_type
            if SwigTypeMapper.is_string_type(out_type):
                continue
            if not self._should_generate_bridge(method):
                continue

            # 构建参数签名 — 使用 SWIG 内部类型解析后的命名空间限定名
            # SWIG 通过 using 声明将类型解析为当前命名空间成员：
            #   using ExportNS::DataObj; → SWIG 视为 PluginNS::DataObj
            # 但全局命名空间的类型保持无限定：
            #   IDasReadOnlyString（全局） → 保持 IDasReadOnlyString
            # 因此 %feature("nodirector") 签名需要根据类型原始命名空间决定是否加前缀
            param_decls = []
            base_ns = interface.namespace  # 基类所在命名空间

            for param in method.parameters:
                cpp_type = self._get_cpp_type(param.type_info.base_type)

                # 判断类型是否需要加命名空间前缀
                # 规则：
                #   - 接口类型定义在其他命名空间（通过 using 引入）→ base_ns::Type
                #   - 接口类型定义在全局命名空间 → ::Type
                #     （基类头文件使用 ::IDasBase** 形式，SWIG 内部类型为 ::IDasBase，
                #      %feature("nodirector") 签名必须匹配，否则 feature 不生效）
                #   - 接口类型与基类同命名空间 → 保持无限定
                #   - 非接口类型 → 保持无限定
                if param.type_info.type_kind == TypeKind.INTERFACE:
                    type_ns = self._get_type_namespace(param.type_info.base_type)
                    if type_ns and type_ns != base_ns:
                        # 类型定义在其他命名空间，通过 using 引入 → SWIG 解析为 base_ns::Type
                        qualified_type = f'{base_ns}::{cpp_type}' if base_ns else cpp_type
                    elif not type_ns:
                        # 类型定义在全局命名空间 → 需要 :: 前缀匹配 SWIG 解析结果
                        qualified_type = f'::{cpp_type}'
                    else:
                        # 类型与基类同命名空间 → 保持无限定
                        qualified_type = cpp_type
                else:
                    qualified_type = cpp_type

                if param.direction == ParamDirection.OUT:
                    if param.type_info.type_kind == TypeKind.INTERFACE:
                        param_decls.append(f'{qualified_type}** {param.name}')
                    else:
                        ptr_type = f'{qualified_type}{"*" * param.type_info.pointer_level}'
                        param_decls.append(f'{ptr_type} {param.name}')
                else:
                    if param.type_info.type_kind == TypeKind.INTERFACE:
                        param_decls.append(f'{qualified_type}* {param.name}')
                    elif param.type_info.is_const and param.type_info.is_reference:
                        param_decls.append(f'const {qualified_type}& {param.name}')
                    elif param.type_info.is_pointer:
                        stars = '*' * param.type_info.pointer_level
                        const_prefix = 'const ' if param.type_info.is_const else ''
                        param_decls.append(f'{const_prefix}{qualified_type}{stars} {param.name}')
                    elif param.type_info.is_reference:
                        param_decls.append(f'{qualified_type}& {param.name}')
                    else:
                        param_decls.append(f'{qualified_type} {param.name}')

            params_str = ', '.join(param_decls)

            # 使用基类命名空间限定的方法名
            if interface.namespace:
                fqn = f'{interface.namespace}::{interface.name}'
            else:
                fqn = interface.name

            if clear:
                lines.append(f'%feature("nodirector", "0") {fqn}::{method.name}({params_str});')
            else:
                lines.append(f'%feature("nodirector") {fqn}::{method.name}({params_str});')

        if lines:
            if clear:
                header = '// Clear base class nodirector features to prevent global side effects'
                return header + '\n' + '\n'.join(lines)
            return '\n'.join(lines)
        return ''

    def _generate_ignore_for_raw_overrides(self, interface: InterfaceDef, swig_name: str) -> str:
        """为 ISwig 类的 raw override 方法生成控制指令

        raw override 方法（3-param 版本）在 ISwig 类中覆写基类纯虚方法，
        内部委托给 2-param 的 DasRetXxx 版本。

        不再使用 %feature("nodirector")，因为 SWIG 4.4.1 的 nodirector
        对非纯虚方法也会生成 throw-pure-virtual 空壳。

        改为让 SWIG 为这些方法生成完整的 Director JNI 回调，
        配合 directorout typemap 正确传递 [out] 参数。
        """
        return ''

    def _generate_directorout_typemaps(self, interface: InterfaceDef) -> str:
        """为 DasRetXxx 接口类型生成 %typemap(directorout)

        DasRetXxx 接口类型现在有正确的拷贝构造函数（带 AddRef/Release），
        不再需要特殊的 directorout typemap。
        """
        return ''

    def _all_documents_interfaces(self):
        """获取所有文档中的接口（当前文档 + 导入的文档）"""
        interfaces = list(self.document.interfaces)
        for doc in self._imported_documents.values():
            interfaces.extend(doc.interfaces)
        return interfaces

    # IDasBase 的方法名 — 这些在所有 ISwig 类的 %inline 块中都有实现
    _IDASBASE_IMPLEMENTED_METHODS = {'AddRef', 'Release', 'QueryInterface'}

    def _is_fully_non_abstract(self, interface: InterfaceDef) -> bool:
        """检查接口及其整个继承链中的所有纯虚方法是否都被覆盖

        当所有基类虚方法都被 nodirector 时，SWIG 会认为类是抽象的（Warning 517），
        不生成默认构造函数。只有当 %inline 块中的代码真正覆盖了继承链中所有纯虚方法时，
        才应该使用 %feature("notabstract")。

        方法被"覆盖"的条件：
        1. 有 bridge 桥接（带 [out] 参数且满足桥接条件） — 2-param override 实现了基类纯虚方法
        2. 在 %inline 类中被显式实现（AddRef/Release/QueryInterface — 所有 ISwig 类都有）

        Args:
            interface: 要检查的接口

        Returns:
            True 表示该接口的 ISwig 类真正非抽象，可以安全使用 %feature("notabstract")
        """
        # 构建接口名称到 InterfaceDef 的映射
        interface_map = {}
        for iface in self._all_documents_interfaces():
            interface_map[iface.name] = iface

        # 收集整个继承链中的所有方法（包括属性生成的 getter/setter）
        all_methods = []
        visited = set()
        current = interface.name
        max_depth = 20  # 防止循环继承

        while current and current not in visited and max_depth > 0:
            visited.add(current)
            max_depth -= 1

            iface_def = interface_map.get(current)
            if not iface_def:
                break

            all_methods.extend(iface_def.methods)
            # 属性会生成 getter（和可能的 setter），这些也是纯虚方法
            for prop in iface_def.properties:
                if prop.has_getter:
                    all_methods.append(MethodDef(
                        name=f'Get{prop.name}',
                        return_type=prop.type_info,
                        parameters=[],
                        is_pure_virtual=True,
                        namespace=iface_def.namespace,
                    ))
                if prop.has_setter:
                    all_methods.append(MethodDef(
                        name=f'Set{prop.name}',
                        return_type=TypeInfo(base_type='void'),
                        parameters=[ParameterDef(
                            name='value',
                            type_info=prop.type_info,
                            direction=ParamDirection.IN,
                        )],
                        is_pure_virtual=True,
                        namespace=iface_def.namespace,
                    ))

            current = iface_def.base_interface
            if current == iface_def.name:
                break

        # 检查每个方法是否被覆盖
        for method in all_methods:
            if self._is_method_covered(method):
                continue
            # 该方法没有被覆盖 → 类仍然是抽象的
            return False

        return True

    def _is_method_covered(self, method: MethodDef) -> bool:
        """检查单个方法是否在 ISwig 类的 %inline 块中被覆盖

        覆盖方式：
        1. AddRef/Release/QueryInterface — 所有 ISwig 类都在 %inline 中实现了（final/override）
        2. 有 [out] 参数的桥接方法 — bridge override 实现了基类纯虚方法
        """
        # IDasBase 的方法总是在 %inline 中实现
        if method.name in self._IDASBASE_IMPLEMENTED_METHODS:
            return True

        # 检查是否满足 bridge 条件（与 _generate_director_bridge_methods 相同的逻辑）
        out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
        if len(out_params) != 1:
            return False

        if method.attributes and method.attributes.get('binary_buffer', False):
            return False

        out_type = out_params[0].type_info.base_type
        if SwigTypeMapper.is_string_type(out_type):
            return False

        if not self._should_generate_bridge(method):
            return False

        return True

    def _all_documents_enums(self):
        """获取所有文档中的枚举（当前文档 + 导入的文档）"""
        enums = list(self.document.enums)
        for doc in self._imported_documents.values():
            enums.extend(doc.enums)
        return enums

    def _is_enum_type(self, type_name: str) -> bool:
        """判断类型是否是枚举类型"""
        return any(e.name == type_name for e in self._all_documents_enums())

    def _is_struct_type(self, type_name: str) -> bool:
        """判断类型是否是结构体类型"""
        all_structs = list(self.document.structs)
        for doc in self._imported_documents.values():
            all_structs.extend(doc.structs)
        return any(s.name == type_name for s in all_structs)

    def _generate_ref_impl_class(self, interface: InterfaceDef) -> str:
        """生成引用计数实现基类（供目标语言继承）"""
        swig_name = self._get_swig_interface_name(interface.name)

        # ISwig 接口应该继承当前接口，而不是基接口
        # 使用当前接口的完全限定名称作为基类
        if interface.namespace:
            base_class_name = f"{interface.namespace}::{interface.name}"
        else:
            # 接口在全局命名空间，需要显式使用::
            base_class_name = f"::{interface.name}"



        # 如果接口有命名空间，引用计数类需要定义在同一个命名空间中
        if interface.namespace:
            if interface.name == 'IDasBase':
                # 特例处理 IDasBase 接口
                interface.name = '::IDasBase'

            # 生成 director 桥接方法（用于 Java 等语言通过 DasRetXxx 覆盖 out 参数方法）
            bridge_methods = self._generate_director_bridge_methods(interface)
            nodirector_directives = self._generate_nodirector_for_raw_methods(interface, swig_name)
            directorout_typemaps = self._generate_directorout_typemaps(interface)
            ignore_raw_overrides = self._generate_ignore_for_raw_overrides(interface, swig_name)

            # 生成基类 nodirector（必须在 %inline 前，紧邻当前 ISwig 类的 Director 构建）
            base_nodirector = self._generate_nodirector_for_base_class(interface)
            # 生成清除指令（必须在 %inline 后立即调用，防止全局污染其他 .i 文件）
            clear_nodirector = self._generate_nodirector_for_base_class(interface, clear=True)

            # 构建 director/nodirector 指令（必须在 %inline 之前）
            director_directive = f'%feature("director") {interface.namespace}::{swig_name};'

            # 当 %inline 块中的代码真正覆盖了继承链中所有纯虚方法时，
            # SWIG 仍可能因 nodirector 导致抽象检查失败（Warning 517），
            # 不生成默认构造函数。此时需要 %feature("notabstract") 强制 SWIG 视为非抽象。
            # 注意：仅对真正非抽象的类使用，否则 C++ 编译会因实例化抽象类而报错。
            notabstract_directive = ''
            if base_nodirector and self._is_fully_non_abstract(interface):
                notabstract_directive = f'\n%feature("notabstract") {interface.namespace}::{swig_name};'

            nodirector_block = ''
            if nodirector_directives:
                nodirector_block = f'\n// Disable director for raw override methods (delegates to 2-param version)\n{nodirector_directives}'

            # 基类 nodirector 块（紧邻 director 指令，限制作用范围）
            base_nodirector_block = ''
            if base_nodirector:
                base_nodirector_block = f'\n// Disable director for base class methods with [out] parameters\n{base_nodirector}'

            # 清除块（紧跟 %inline 之后）
            clear_nodirector_block = ''
            if clear_nodirector:
                clear_nodirector_block = f'\n{clear_nodirector}\n'

            return f"""
// Reference counting implementation base class for {interface.name}
// Target language should inherit from this class
// Exported as {swig_name} to SWIG

{directorout_typemaps}

// Enable director for SWIG wrapper class — must come before %inline
{director_directive}{notabstract_directive}
{base_nodirector_block}
{nodirector_block}

%inline %{{

namespace {interface.namespace} {{

 DAS_SWIG_DIRECTOR_ATTRIBUTE({swig_name})
class {swig_name} : public {base_class_name} {{
private:
    std::atomic<uint32_t> ref_count_{{1}};

public:
    virtual ~{swig_name}() = default;

    // 纯虚函数声明（从基类继承）
    // 注意：这些方法由基类提供，无需在此重新声明

    // 实现引用计数方法（标记为 final）
    uint32_t AddRef() final {{
        return ++ref_count_;
    }}

    uint32_t Release() final {{
        const auto result = --ref_count_;
        if (result == 0) {{
            delete this;
        }}
        return result;
    }}

    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override {{
        if (IsDasGuidEqual(iid, DasIidOf<{interface.namespace}::{interface.name}>())) {{
            AddRef();
            *pp_out_object = static_cast<{interface.name}*>(this);
            return DAS_S_OK;
        }}
        if (IsDasGuidEqual(iid, DasIidOf<::IDasBase>())) {{
            AddRef();
            *pp_out_object = static_cast<::IDasBase*>(this);
            return DAS_S_OK;
        }}
        *pp_out_object = nullptr;
        return DAS_E_NO_INTERFACE;
    }}
{bridge_methods}
}};

}} // namespace {interface.namespace}

%}}
{clear_nodirector_block}
// Hide reference counting methods from {swig_name}
%ignore {interface.namespace}::{swig_name}::AddRef;
%ignore {interface.namespace}::{swig_name}::Release;
%ignore {interface.namespace}::{swig_name}::QueryInterface;

{ignore_raw_overrides}
"""
        else:
            # 无命名空间的版本
            bridge_methods = self._generate_director_bridge_methods(interface)
            nodirector_directives = self._generate_nodirector_for_raw_methods(interface, swig_name)
            directorout_typemaps = self._generate_directorout_typemaps(interface)
            ignore_raw_overrides = self._generate_ignore_for_raw_overrides(interface, swig_name)

            # 生成基类 nodirector（必须在 %inline 前，紧邻当前 ISwig 类的 Director 构建）
            base_nodirector = self._generate_nodirector_for_base_class(interface)
            # 生成清除指令（必须在 %inline 后立即调用，防止全局污染其他 .i 文件）
            clear_nodirector = self._generate_nodirector_for_base_class(interface, clear=True)

            # 无命名空间版本：构建 director/nodirector 指令（必须在 %inline 之前）
            director_directive = f'%feature("director") {swig_name};'
            nodirector_block = ''
            if nodirector_directives:
                nodirector_block = f'\n// Disable director for raw override methods (delegates to 2-param version)\n{nodirector_directives}'

            # 基类 nodirector 块（紧邻 director 指令，限制作用范围）
            base_nodirector_block = ''
            if base_nodirector:
                base_nodirector_block = f'\n// Disable director for base class methods with [out] parameters\n{base_nodirector}'

            # 清除块（紧跟 %inline 之后）
            clear_nodirector_block = ''
            if clear_nodirector:
                clear_nodirector_block = f'\n{clear_nodirector}\n'

            return f"""
// Reference counting implementation base class for {interface.name}
// Target language should inherit from this class
// Exported as {swig_name} to SWIG

{directorout_typemaps}

// Enable director for the SWIG wrapper class — must come before %inline
{director_directive}
{base_nodirector_block}
{nodirector_block}

%inline %{{

class {swig_name} : public {base_class_name} {{
private:
    std::atomic<uint32_t> ref_count_{{1}};

public:
    virtual ~{swig_name}() = default;

    // 纯虚函数声明（从基类继承）
    // 注意：这些方法由基类提供，无需在此重新声明

    // 实现引用计数方法（标记为 final）
    uint32_t AddRef() final {{
        return ++ref_count_;
    }}

    uint32_t Release() final {{
        const auto result = --ref_count_;
        if (result == 0) {{
            delete this;
        }}
        return result;
    }}

    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override {{
        if (IsDasGuidEqual(iid, DasIidOf<{interface.name}>())) {{
            AddRef();
            *pp_out_object = static_cast<{interface.name}*>(this);
            return DAS_S_OK;
        }}
        if (IsDasGuidEqual(iid, DasIidOf<::IDasBase>())) {{
            AddRef();
            *pp_out_object = static_cast<::IDasBase*>(this);
            return DAS_S_OK;
        }}
        *pp_out_object = nullptr;
        return DAS_E_NO_INTERFACE;
    }}
{bridge_methods}
}};

%}}
{clear_nodirector_block}
// Hide reference counting methods from {swig_name}
%ignore {swig_name}::AddRef;
%ignore {swig_name}::Release;
%ignore {swig_name}::QueryInterface;

{ignore_raw_overrides}
"""
    def _get_abi_header_file(self, interface_name: str) -> str:
        """获取接口对应的 ABI 头文件路径"""
        if self.idl_file_name:
            return os.path.splitext(self.idl_file_name)[0]
        else:
            return interface_name

    def _get_abi_includes(self, interface_name: str) -> List[str]:
        """获取所有需要的 ABI 头文件（包括导入的 IDL 文件的头文件）"""
        includes = []

        # 添加当前接口的 ABI 头文件
        abi_header = self._get_abi_header_file(interface_name)
        includes.append(f"{abi_header}.h")

        # 添加所有导入的 IDL 文件的 ABI 头文件
        for import_def in self.document.imports:
            # 从导入路径中提取 IDL 文件名
            import_path = import_def.idl_path.strip('"')
            import_filename = Path(import_path).stem
            includes.append(f"{import_filename}.h")

        return includes

    def generate_interface_i_file(self, interface: InterfaceDef, output_typemap_info: bool = False, task_id: str = "") -> str:
        """生成单个接口的 .i 文件内容"""
        self._collect_interface_types(interface)
        # 重置 typemap 收集列表
        self._current_typemaps = []

        # 收集所有接口列表（包括导入的文档）
        all_interfaces = []
        all_interfaces.extend(self.document.interfaces)
        for doc in self._imported_documents.values():
            all_interfaces.extend(doc.interfaces)

        # 收集所有枚举列表（包括导入的文档）
        all_enums = []
        all_enums.extend(self.document.enums)
        for doc in self._imported_documents.values():
            all_enums.extend(doc.enums)

        # 为 Java 生成器设置所有接口和枚举列表
        for lang_generator in self.lang_generators:
            if lang_generator.__class__.__name__ == 'JavaSwigGenerator':
                lang_generator.set_all_interfaces(all_interfaces)
                lang_generator.set_all_enums(all_enums)

        # ============================================================================
        # 构建 SWIG 接口分析模型（Phase 1 集成）
        # ============================================================================
        # 构建接口映射用于继承链解析
        interface_map = build_interface_map(all_interfaces)
        
        # 构建当前接口的模型
        interface_model = build_swig_interface_model(interface, interface_map)
        
        # 通知所有语言生成器处理模型（扩展点）
        for lang_generator in self.lang_generators:
            lang_generator.on_interface_model(interface_model, interface)
        
        if self.debug:
            print(f"[DEBUG] Built SwigInterfaceModel for {interface.name}: "
                  f"inherits_idas_type_info={interface_model.inherits_idas_type_info}, "
                  f"out_methods={len(interface_model.out_methods)}, "
                  f"multi_out_methods={len(interface_model.multi_out_methods)}")

        lines = []
        lines.append(self._file_header(interface.name))

        abi_includes = self._get_abi_includes(interface.name)
        # %{ %} 块 - 包含 C++ 头文件
        header_lines = []
        header_lines.append("%{")
        header_lines.append(f"#include <atomic>")
        for abi_include in abi_includes:
            header_lines.append(f"#include <{abi_include}>")
        header_lines.append("%}")
        header_block = "\n".join(header_lines)
        lines.append(header_block)
        lines.append("")

        # 收集 header block 到全局字典（用于 DasTypeMaps.i）
        if interface.name not in self._global_header_blocks:
            self._global_header_blocks[interface.name] = header_block
            if self.debug:
                print(f"[DEBUG] Collected header block for {interface.name}")

        # 生成语言特定的预处理指令（%rename、%javamethodmodifiers、%typemap(javacode) 等）
        # 这些指令必须在 %include 之前才能正确生效
        for lang_generator in self.lang_generators:
            pre_include_code = lang_generator.generate_pre_include_directives(interface)
            if pre_include_code:
                lines.append(pre_include_code)

        # 为二进制缓冲区接口生成特殊的 typemap
        # 必须在 %include 之前生成，这样 SWIG 才能在处理头文件时应用这些 typemap
        lines.append(self._generate_binary_buffer_typemaps(interface))

        # %include 指令 - SWIG 形式 include
        same_name_include = None
        for abi_include in abi_includes:
            if abi_include.startswith(f"{interface.name}."):
                same_name_include = abi_include
            else:
                lines.append(f"%include <{abi_include}>")

        if same_name_include:
            lines.append(f"%include <{same_name_include}>")

        # 为跨命名空间类型注入 wrapper 代码中的 using 声明
        # SWIG 在 #ifdef SWIG 分支中将不带限定的类型解析到当前命名空间，
        # 导致生成的 wrapper 代码中出现 NamespaceA::TypeFromNamespaceB 形式。
        # 通过在 wrapper 代码中注入 using 声明，让 C++ 编译器能正确解析。
        wrapper_usings = self._generate_cross_namespace_wrapper_usings(interface)
        if wrapper_usings:
            lines.append(wrapper_usings)

        # 生成语言特定的 [out] 参数包装代码（包含 DasRetXxx 类型定义）
        # 放在 %include 之后，确保 SWIG 已经看到所有类型定义
        lang_specific_code = self._generate_lang_specific_out_param_wrappers(interface)
        if lang_specific_code:
            lines.append(lang_specific_code)

        # 生成 IID 静态方法（%extend）
        lines.append(self._generate_iid_static_method(interface))

        # 生成语言特定的后置指令（javacode typemap）
        # 放在 IID 方法之后，这是用户指定的位置
        for lang_generator in self.lang_generators:
            post_include_code = lang_generator.generate_post_include_directives(interface)
            if post_include_code:
                lines.append(post_include_code)

        # 调用新的 emit_post_include 扩展点（基于 SwigInterfaceModel）
        # 这是 Phase 1 集成新增的方法，用于生成基于模型的后置代码
        for lang_generator in self.lang_generators:
            post_code = lang_generator.emit_post_include(interface_model, interface)
            if post_code:
                lines.append(post_code)

        # ignore 指令（原始接口）
        lines.append(self._generate_ignore_directives(interface))
        lines.append("")

        # director 指令（原始接口，用于 C++ 回调）
        # lines.append(self._generate_director_directive(interface))
        # lines.append("")

        # 生成 Director out 参数 typemap（在 Director 类定义之前）
        director_typemaps = self._generate_director_out_param_typemaps(interface)
        if director_typemaps:
            lines.append("\n// Director out 参数 typemap\n")
            lines.extend(director_typemaps.split("\n"))
            lines.append("")
        
        
        # 生成引用计数实现基类 (IDasSwigXxx)
        lines.append(self._generate_ref_impl_class(interface))

        # 在文件末尾清除所有 typemap
        lines.append(self._generate_clear_typemaps())

        result = "\n".join(lines)

        return result

def generate_swig_files(document: IdlDocument, output_dir: str, base_name: str, idl_file_path: Optional[str] = None, output_typemap_info: bool = False, task_id: str = "", debug: bool = False) -> List[str]:
    """生成所有 SWIG .i 文件（不包含_all.i文件，由CMake生成）"""
    import os

    # 提取IDL文件名
    idl_file_name = None
    if idl_file_path:
        idl_file_name = os.path.basename(idl_file_path)

    generator = SwigCodeGenerator(document, idl_file_name, idl_file_path, debug=debug)
    generated_files = []

    # 确保输出目录存在
    os.makedirs(output_dir, exist_ok=True)

    # 为每个接口生成单独的 .i 文件
    for interface in document.interfaces:
        i_content = generator.generate_interface_i_file(interface, output_typemap_info=output_typemap_info, task_id=task_id)
        i_filename = f"{interface.name}.i"
        i_path = os.path.join(output_dir, i_filename)

        with open(i_path, 'w', encoding='utf-8') as f:
            f.write(i_content)

        print(f"Generated: {i_path}")
        generated_files.append(i_path)

    # 不再生成_all.i文件，由CMake在代码生成后统一生成

    # 生成 typemap_info.json 文件（所有接口完成后统一生成）
    if output_typemap_info:
        # 使用 output_dir 参数指定的目录，而不是硬编码的源代码目录
        output_path = Path(output_dir)

        typemaps_info = {}
        for sig, code in generator._global_typemaps.items():
            typemaps_info[sig] = {
                "code": code,
                "meta": {
                    "signature": sig,
                    "origin": idl_file_name or "unknown"
                }
            }

        ret_classes_info = {}
        for class_name, class_code in generator._global_ret_classes.items():
            ret_classes_info[class_name] = {
                "code": class_code,
                "meta": {
                    "origin": idl_file_name or "unknown"
                }
            }

        header_blocks_info = {}
        for interface_name, header_block in generator._global_header_blocks.items():
            header_blocks_info[interface_name] = {
                "code": header_block,
                "meta": {
                    "origin": idl_file_name or "unknown"
                }
            }

        typemaps_ignore_info = {}
        if hasattr(generator, '_global_typemaps_ignore'):
            for sig, code in generator._global_typemaps_ignore.items():
                typemaps_ignore_info[sig] = {
                    "code": code,
                    "meta": {
                        "signature": sig,
                        "origin": idl_file_name or "unknown"
                    }
                }

        typemap_info = {
            "schema_version": "1.0",
            "generator": {
                "name": "das_idl_gen",
                "version": "1.0.0"
            },
            "typemaps": typemaps_info,
            "typemaps_ignore": typemaps_ignore_info,
            "ret_classes": ret_classes_info,
            "header_blocks": header_blocks_info
        }

        if task_id:
            json_file = output_path / f"typemap_info_{task_id}.json"
        else:
            json_file = output_path / "typemap_info.json"
        json_file.parent.mkdir(parents=True, exist_ok=True)
        with open(json_file, 'w', encoding='utf-8') as f:
            json.dump(typemap_info, f, indent=2, ensure_ascii=False)

        print(f"Generated: {json_file}")

    return generated_files


# 测试代码
if __name__ == '__main__':
    from das_idl_parser import parse_idl

    test_idl = '''
    [uuid("D5B3213B-B7C4-4194-0E99-5CEFFF064F8D")]
    interface IDasGuidVector : IDasBase {
        DasResult Size([out] size_t* p_out_size);
        DasResult At(size_t index, [out] DasGuid* p_out_iid);
        DasResult Find(const DasGuid& iid);
        DasResult PushBack(const DasGuid& iid);
    }
    '''

    doc = parse_idl(test_idl)
    generator = SwigCodeGenerator(doc)

    print("=== 生成的 .i 文件 ===")
    for interface in doc.interfaces:
        print(generator.generate_interface_i_file(interface))
