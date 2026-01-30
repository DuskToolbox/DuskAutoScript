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

import os
from datetime import datetime, timezone
from pathlib import Path
from typing import List, Optional, Set, Dict
from das_idl_parser import (
    IdlDocument, InterfaceDef, EnumDef, MethodDef, PropertyDef,
    ParameterDef, TypeInfo, ParamDirection
)
from das_idl_parser import parse_idl_file as _das_idl_parser_parse_idl_file
from swig_java_generator import JavaSwigGenerator
from swig_csharp_generator import CSharpSwigGenerator
from swig_python_generator import PythonSwigGenerator

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
    def is_interface_type(type_name: str) -> bool:
        """判断是否是接口类型（支持带命名空间的类型名）"""
        # 提取简单名称（去除命名空间限定符）
        simple_name = type_name.split('::')[-1]
        return simple_name.startswith('I') and len(simple_name) > 1 and simple_name[1:2].isupper()

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
        if cls.is_interface_type(base):
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

    def __init__(self, document: IdlDocument, idl_file_name: Optional[str] = None, idl_file_path: Optional[str] = None, lang_generators: Optional[List] = None):
        self.document = document
        self.idl_file_name = idl_file_name
        self.idl_file_path = idl_file_path
        self.indent = "    "
        self._collected_interface_types: Set[str] = set()
        self._current_typemaps: List[str] = []
        self._imported_documents: Dict[str, IdlDocument] = {}
        self._parse_imported_documents()

        if lang_generators is None:
            self.lang_generators = [JavaSwigGenerator(), CSharpSwigGenerator(), PythonSwigGenerator()]
        else:
            self.lang_generators = lang_generators

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

    def _is_binary_data_method(self, interface: InterfaceDef, method: MethodDef) -> bool:
        """判断是否是 binary_buffer 方法"""
        if method.name == 'GetData' and self._is_binary_buffer_interface(interface):
            return True
        return False

        for method in interface.methods:
            if method.attributes.get('binary_buffer', False):
                return True
        return False

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

    def _is_binary_data_method(self, interface: InterfaceDef, method: MethodDef) -> bool:
        """判断是否是返回二进制数据的方法

        [binary_buffer] 方法必须满足：
        1. 有 [binary_buffer] 属性标记
        2. 输出参数类型为 unsigned char** 或 const unsigned char**
        """
        return method.attributes.get('binary_buffer', False)

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
                    if SwigTypeMapper.is_interface_type(param.type_info.base_type):
                        self._collected_interface_types.add(param.type_info.base_type)

        for prop in interface.properties:
            if SwigTypeMapper.is_interface_type(prop.type_info.base_type):
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

    def _generate_out_param_typemap(self, interface: InterfaceDef, method: MethodDef, param: ParameterDef) -> str:
        """生成 [out] 参数的 typemap"""
        if self._is_binary_data_method(interface, method):
            return ""
        base_type = param.type_info.base_type
        is_interface = SwigTypeMapper.is_interface_type(base_type)

        qualified_interface_name = self._get_qualified_name(interface.name, interface.namespace)
        lines = []
        lines.append(f"// {qualified_interface_name}::{method.name} - {param.name} parameter")

        if is_interface:
            typemap_sig = f"{base_type}** {param.name}"
            self._current_typemaps.append(typemap_sig)
            lines.append(f"""
%typemap(in, numinputs=0) {typemap_sig} ({base_type}* temp_{param.name} = nullptr) {{
    $1 = &temp_{param.name};
}}
%typemap(argout) {typemap_sig} {{
    $result = SWIG_NewPointerObj(SWIG_as_voidptr(temp_{param.name}), $descriptor({base_type}*), SWIG_POINTER_OWN);
}}
""")
        else:
            cpp_type = self._get_cpp_type(base_type)
            typemap_sig = f"{cpp_type}* {param.name}"
            self._current_typemaps.append(typemap_sig)
            lines.append(f"""
%typemap(in, numinputs=0) {typemap_sig} ({cpp_type} temp_{param.name}) {{
    $1 = &temp_{param.name};
}}
%typemap(argout) {typemap_sig} {{
    %append_output(SWIG_From_{self._get_swig_from_type(base_type)}(temp_{param.name}));
}}
""")

        lang_codes = []
        for lang_generator in self.lang_generators:
            lang_code = lang_generator.generate_out_param_wrapper(interface, method, param)
            if lang_code:
                lang_codes.append(lang_code)

        lines.append("\n".join(lang_codes))
        return "\n".join(lines)

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

        lines.append("\n".join(lang_codes))
        return "\n".join(lines)

    def _generate_clear_typemaps(self) -> str:
        """生成 %clear 指令，清除所有已定义的 typemap"""
        if not self._current_typemaps:
            return ""

        lines = []
        lines.append("// Clear typemaps to avoid redefinition in other interfaces")
        for typemap_sig in self._current_typemaps:
            lines.append(f"%clear {typemap_sig};")
        lines.append("")

        return "\n".join(lines)

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
            return f"""
// Reference counting implementation base class for {interface.name}
// Target language should inherit from this class
// Exported as {swig_name} to SWIG
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
}};

}} // namespace {interface.namespace}

%}}

// Enable director for SWIG wrapper class
%feature("director") {interface.namespace}::{swig_name};

// Hide reference counting methods from {swig_name}
%ignore {interface.namespace}::{swig_name}::AddRef;
%ignore {interface.namespace}::{swig_name}::Release;
%ignore {interface.namespace}::{swig_name}::QueryInterface;
"""
        else:
            # 无命名空间的版本
            return f"""
// Reference counting implementation base class for {interface.name}
// Target language should inherit from this class
// Exported as {swig_name} to SWIG
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
}};

%}}

// Enable director for the SWIG wrapper class
%feature("director") {swig_name};

// Hide reference counting methods from {swig_name}
%ignore {swig_name}::AddRef;
%ignore {swig_name}::Release;
%ignore {swig_name}::QueryInterface;
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

    def generate_interface_i_file(self, interface: InterfaceDef) -> str:
        """生成单个接口的 .i 文件内容"""
        self._collect_interface_types(interface)
        # 重置 typemap 收集列表
        self._current_typemaps = []

        lines = []
        lines.append(self._file_header(interface.name))

        abi_includes = self._get_abi_includes(interface.name)
        # %{ %} 块 - 包含 C++ 头文件
        lines.append("%{")
        lines.append(f"#include <atomic>")
        for abi_include in abi_includes:
            lines.append(f"#include <{abi_include}>")
        lines.append("%}")
        lines.append("")

        # %{ %} 块 - SWIG 形式 include
        for abi_include in abi_includes:
            lines.append(f"%include <{abi_include}>")

        # ignore 指令（原始接口）
        lines.append(self._generate_ignore_directives(interface))
        lines.append("")

        # director 指令（原始接口，用于 C++ 回调）
        # lines.append(self._generate_director_directive(interface))
        # lines.append("")

        # 为所有 [out] 参数生成 typemap
        for method in interface.methods:
            for param in method.parameters:
                if param.direction == ParamDirection.OUT:
                    lines.append(self._generate_out_param_typemap(interface, method, param))

        # 生成引用计数实现基类 (IDasSwigXxx)
        lines.append(self._generate_ref_impl_class(interface))

        # 为二进制缓冲区接口生成特殊的 typemap
        lines.append(self._generate_binary_buffer_typemaps(interface))

        # 在文件末尾清除所有 typemap
        lines.append(self._generate_clear_typemaps())

        return "\n".join(lines)

    def generate_all_i_file(self, base_name: str, i_files: List[str], header_file: str) -> str:
        """生成汇总的 .i 文件（已弃用，由CMake生成）"""
        # 此方法已弃用，现在由CMake在代码生成后统一生成_all.i文件
        return ""



def generate_swig_files(document: IdlDocument, output_dir: str, base_name: str, idl_file_path: Optional[str] = None) -> List[str]:
    """生成所有 SWIG .i 文件（不包含_all.i文件，由CMake生成）"""
    import os

    # 提取IDL文件名
    idl_file_name = None
    if idl_file_path:
        idl_file_name = os.path.basename(idl_file_path)

    generator = SwigCodeGenerator(document, idl_file_name, idl_file_path)
    generated_files = []

    # 确保输出目录存在
    os.makedirs(output_dir, exist_ok=True)

    # 为每个接口生成单独的 .i 文件
    for interface in document.interfaces:
        i_content = generator.generate_interface_i_file(interface)
        i_filename = f"{interface.name}.i"
        i_path = os.path.join(output_dir, i_filename)

        with open(i_path, 'w', encoding='utf-8') as f:
            f.write(i_content)

        print(f"Generated: {i_path}")
        generated_files.append(i_path)

    # 不再生成_all.i文件，由CMake在代码生成后统一生成

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
