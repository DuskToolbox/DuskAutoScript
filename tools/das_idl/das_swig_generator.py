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

# 二进制缓冲区接口列表（需要特殊处理的接口）
BINARY_BUFFER_INTERFACES = {'IDasMemory', 'IDasImage'}

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

    def __init__(self, document: IdlDocument, idl_file_name: Optional[str] = None, idl_file_path: Optional[str] = None):
        self.document = document
        self.idl_file_name = idl_file_name  # IDL文件名
        self.idl_file_path = idl_file_path  # IDL文件路径
        self.indent = "    "
        # 收集所有需要的接口类型（用于生成 DAS_DEFINE_RET_POINTER）
        self._collected_interface_types: Set[str] = set()
        # 收集当前接口的所有 typemap，用于最后清除
        self._current_typemaps: List[str] = []
        # 解析导入的 IDL 文档
        self._imported_documents: Dict[str, IdlDocument] = {}
        self._parse_imported_documents()

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

    def _is_binary_buffer_interface(self, interface_name: str) -> bool:
        """判断是否是二进制缓冲区接口"""
        return interface_name in BINARY_BUFFER_INTERFACES

    def _get_interface_namespace(self, interface_name: str) -> str | None:
        """根据接口名查找其命名空间

        Args:
            interface_name: 接口名称（可以是简单名或完全限定名）

        Returns:
            命名空间字符串，如果未找到则返回 None
        """
        # 提取简单名称
        simple_name = interface_name.split('::')[-1]

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
        # 跳过binary_buffer方法的typemap生成
        # binary_buffer方法已经有专门的_binary_buffer_typemaps方法处理
        if self._is_binary_data_method(interface, method):
            return ""
        base_type = param.type_info.base_type
        is_interface = SwigTypeMapper.is_interface_type(base_type)

        # 获取带命名空间的限定名称
        qualified_interface_name = self._get_qualified_name(interface.name, interface.namespace)

        lines = []

        # 根据类型生成不同的 typemap
        if is_interface:
            # 接口类型：pp_out 参数
            typemap_sig = f"{base_type}** {param.name}"
            self._current_typemaps.append(typemap_sig)
            lines.append(f"""
// Typemap for {qualified_interface_name}::{method.name} - {param.name} parameter
%typemap(in, numinputs=0) {typemap_sig} ({base_type}* temp_{param.name} = nullptr) {{
    $1 = &temp_{param.name};
}}
%typemap(argout) {typemap_sig} {{
    // Return the interface pointer, SWIG will handle the wrapping
    $result = SWIG_NewPointerObj(SWIG_as_voidptr(temp_{param.name}), $descriptor({base_type}*), SWIG_POINTER_OWN);
}}
""")
        else:
            # 基本类型：p_out 参数
            cpp_type = self._get_cpp_type(base_type)
            typemap_sig = f"{cpp_type}* {param.name}"
            self._current_typemaps.append(typemap_sig)
            lines.append(f"""
// Typemap for {qualified_interface_name}::{method.name} - {param.name} parameter
%typemap(in, numinputs=0) {typemap_sig} ({cpp_type} temp_{param.name}) {{
    $1 = &temp_{param.name};
}}
%typemap(argout) {typemap_sig} {{
    // Return the output value
    %append_output(SWIG_From_{self._get_swig_from_type(base_type)}(temp_{param.name}));
}}
""")

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
        if not self._is_binary_buffer_interface(interface.name):
            return ""

        # 验证所有 binary_buffer 方法的参数类型
        for method in interface.methods:
            self._validate_binary_buffer_method(interface, method)

        qualified_name = self._get_qualified_name(interface.name, interface.namespace)
        native_name = f'{interface.name}_createDirectByteBuffer'
        lines = []

        lines.append(f"""
// ============================================================================
// Binary Buffer Support for {interface.name}
// 为二进制数据提供目标语言友好的访问方式
// [binary_buffer] 方法参数类型: unsigned char** (统一类型，简化处理)
// ============================================================================

// Python: 提供 memoryview 零拷贝视图
#ifdef SWIGPYTHON
%extend {qualified_name} {{
    PyObject* GetDataAsMemoryView() {{
        unsigned char* data = nullptr;
        uint64_t size = 0;

        // 获取数据指针 (unsigned char**)
        DasResult hr = $self->GetData(&data);
        if (hr < 0) {{
            PyErr_SetString(PyExc_RuntimeError, "Failed to get data pointer");
            return nullptr;
        }}

        // 获取数据大小
        hr = $self->GetSize(&size);
        if (hr < 0) {{
            PyErr_SetString(PyExc_RuntimeError, "Failed to get data size");
            return nullptr;
        }}

        // 创建 memoryview（零拷贝）
        return PyMemoryView_FromMemory(reinterpret_cast<char*>(data), static_cast<Py_ssize_t>(size), PyBUF_READ);
    }}

    PyObject* GetDataAsBytes() {{
        unsigned char* data = nullptr;
        uint64_t size = 0;

        // 获取数据指针 (unsigned char**)
        DasResult hr = $self->GetData(&data);
        if (hr < 0) {{
            PyErr_SetString(PyExc_RuntimeError, "Failed to get data pointer");
            return nullptr;
        }}

        // 获取数据大小
        hr = $self->GetSize(&size);
        if (hr < 0) {{
            PyErr_SetString(PyExc_RuntimeError, "Failed to get data size");
            return nullptr;
        }}

        // 创建 bytes 对象（拷贝数据）
        return PyBytes_FromStringAndSize(reinterpret_cast<const char*>(data), static_cast<Py_ssize_t>(size));
    }}
}}
#endif // SWIGPYTHON

// C#: 提供 IntPtr 和 Span<byte> 支持
#ifdef SWIGCSHARP
%typemap(csclassmodifiers) {qualified_name} "public partial class"

%typemap(cscode) {qualified_name} %{{
    /// <summary>
    /// 获取数据的原始指针（用于高性能场景）
    /// </summary>
    public System.IntPtr GetDataPointer() {{
        System.IntPtr ptr = System.IntPtr.Zero;
        var result = GetData(out ptr);
        if (result < 0) {{
            throw new System.Exception("Failed to get data pointer");
        }}
        return ptr;
    }}

    /// <summary>
    /// 获取数据的 Span 视图（零拷贝，需要 unsafe 上下文）
    /// </summary>
    public unsafe System.Span<byte> GetDataAsSpan() {{
        var ptr = GetDataPointer();
        ulong size;
        GetSize(out size);
        return new System.Span<byte>(ptr.ToPointer(), (int)size);
    }}

    /// <summary>
    /// 获取数据的字节数组副本
    /// </summary>
    public byte[] GetDataAsByteArray() {{
        var ptr = GetDataPointer();
        ulong size;
        GetSize(out size);
        var array = new byte[size];
        System.Runtime.InteropServices.Marshal.Copy(ptr, array, 0, (int)size);
        return array;
    }}
%}}
#endif // SWIGCSHARP

// Java: 提供 ByteBuffer 支持
#ifdef SWIGJAVA
%typemap(javaclassmodifiers) {qualified_name} "public class"

%typemap(javacode) {qualified_name} %{{
    /**
     * 获取数据的直接 ByteBuffer（零拷贝）
     * @return 直接 ByteBuffer 视图
     */
    public java.nio.ByteBuffer getDataAsDirectBuffer() {{
        long[] ptrHolder = new long[1];
        long[] sizeHolder = new long[1];

        int hr = GetDataNative(ptrHolder);
        if (hr < 0) {{
            throw new RuntimeException("Failed to get data pointer");
        }}

        hr = GetSizeNative(sizeHolder);
        if (hr < 0) {{
            throw new RuntimeException("Failed to get data size");
        }}

        return {native_name}(ptrHolder[0], (int)sizeHolder[0]);
    }}

    private static native java.nio.ByteBuffer {native_name}(long address, int capacity);
%}}

%native({native_name}) jobject {native_name}(jlong address, jint capacity);
%{{
JNIEXPORT jobject JNICALL Java_{qualified_name.replace('::', '_')}_createDirectByteBuffer(
    JNIEnv *jenv, jclass jcls, jlong address, jint capacity) {{
    return jenv->NewDirectByteBuffer((void*)address, capacity);
}}
%}}
#endif // SWIGJAVA
""")

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

    def _collect_inherited_methods(self, interface: InterfaceDef) -> List[tuple]:
        """收集所有继承链上的方法

        返回: List[tuple] - 每个元组包含 (method: MethodDef, interface_name: str, namespace: str)
        """
        all_methods = []
        visited = set()  # 防止循环继承

        # 当前接口的方法
        current_interface = interface
        current_namespace = interface.namespace

        while current_interface:
            # 防止循环继承
            interface_key = (current_interface.name, current_interface.namespace)
            if interface_key in visited:
                break
            visited.add(interface_key)

            # 收集当前接口的所有方法
            for method in current_interface.methods:
                all_methods.append((method, current_interface.name, current_interface.namespace))

            # 获取基类
            base_interface_name = current_interface.base_interface
            if not base_interface_name or base_interface_name == "IDasBase":
                break

            # 查找基类接口
            base_interface = self._find_interface_by_name(base_interface_name, current_interface.namespace)
            if base_interface:
                current_interface = base_interface
            else:
                # 没有找到基类定义，可能是在外部 IDL 中定义的
                # 停止向上查找
                break

        return all_methods

    def _generate_method_declaration(self, method: MethodDef, current_namespace: str = "") -> str:
        """生成单个方法的声明，为接口类型使用完全限定名称"""
        # 返回类型
        ret_type_str = self._format_type_info(method.return_type, current_namespace)

        # 参数列表
        params = []
        for param in method.parameters:
            param_str = f"{self._format_type_info(param.type_info, current_namespace)} {param.name}"
            params.append(param_str)

        params_str = ", ".join(params)

        # 方法声明
        return f"    virtual {ret_type_str} {method.name}({params_str}) = 0;"

    def _format_type_info(self, type_info: TypeInfo, current_namespace: str = "") -> str:
        """格式化类型信息为 C++ 类型字符串，为接口类型使用完全限定名称"""
        parts = []

        if type_info.is_const:
            parts.append("const")

        # 处理复合类型（如 unsigned char）
        base_type = type_info.base_type

        # 添加基础类型（为接口类型添加完全限定名称）
        if SwigTypeMapper.is_interface_type(base_type):
            # 查找接口的命名空间（不使用 current_namespace，直接查找所有文档中的接口）
            interface_def = self._find_interface_by_name(base_type)
            if interface_def and interface_def.namespace:
                qualified_name = f"{interface_def.namespace}::{base_type}"
                parts.append(qualified_name)
            else:
                # 没有找到命名空间，直接使用类型名称
                parts.append(base_type)
        else:
            # 非接口类型，直接使用类型名称
            parts.append(base_type)

        # 处理指针和引用
        if type_info.is_pointer:
            pointer_suffix = "*" * type_info.pointer_level
            parts.append(pointer_suffix)
        elif type_info.is_reference:
            parts.append("&")

        return " ".join(parts)

    def _generate_ref_impl_class(self, interface: InterfaceDef) -> str:
        """生成引用计数实现基类（供目标语言继承）"""
        swig_name = self._get_swig_interface_name(interface.name)

        # 获取基类的完全限定名称（两个分支都需要）
        base_class_name = interface.name
        if interface.base_interface:
            base_namespace = self._get_interface_namespace(interface.base_interface)
            if base_namespace:
                base_class_name = f"{base_namespace}::{interface.base_interface}"
            else:
                # 基类在全局命名空间，需要显式使用::
                base_class_name = f"::{interface.base_interface}"

        # 收集所有继承链上的方法
        inherited_methods = self._collect_inherited_methods(interface)

        # 生成纯虚函数声明（排除 AddRef 和 Release）
        virtual_declarations = []
        for method, method_interface_name, method_namespace in inherited_methods:
            if method.name not in ("AddRef", "Release", "QueryInterface"):
                decl = self._generate_method_declaration(method, method_namespace)
                virtual_declarations.append(decl)
                # 添加注释说明方法来自哪个接口
                if method_interface_name != interface.name:
                    virtual_declarations.append(f"    // Inherited from {method_interface_name}")

        virtual_methods_str = "\n".join(virtual_declarations)

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
{virtual_methods_str}

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
{virtual_methods_str}

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
