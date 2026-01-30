"""
Java SWIG 生成器

生成 Java 特定的 SWIG .i 文件代码
"""

from das_idl_parser import InterfaceDef, MethodDef, ParameterDef, ParamDirection
from swig_lang_generator_base import SwigLangGenerator


class JavaSwigGenerator(SwigLangGenerator):
    """Java 特定的 SWIG 代码生成器"""

    @staticmethod
    def _get_java_type(type_name: str) -> str:
        """获取 Java 类型"""
        JAVA_TYPE_MAP = {
            'bool': 'boolean',
            'int8': 'byte',
            'int16': 'short',
            'int32': 'int',
            'int64': 'long',
            'int64_t': 'long',
            'uint8': 'short',
            'uint16': 'int',
            'uint32': 'long',
            'uint64': 'java.math.BigInteger',
            'uint64_t': 'java.math.BigInteger',
            'float': 'float',
            'double': 'double',
            'size_t': 'long',
            'int': 'int',
            'uint': 'long',
            'DasBool': 'boolean',
            'DasGuid': 'DasGuid',
            'DasString': 'String',
            'DasReadOnlyString': 'String',
            'DasResult': 'DasResult',
        }

        if type_name in JAVA_TYPE_MAP:
            return JAVA_TYPE_MAP[type_name]

        return type_name

    @staticmethod
    def _is_interface_type(type_name: str) -> bool:
        """判断是否是接口类型"""
        simple_name = type_name.split('::')[-1]
        return simple_name.startswith('I') and len(simple_name) > 1 and simple_name[1:2].isupper()

    def get_language_name(self) -> str:
        return 'java'

    def get_swig_define(self) -> str:
        return 'SWIGJAVA'

    def generate_out_param_wrapper(self, interface: InterfaceDef, method: MethodDef, param: ParameterDef) -> str:
        """生成 Java 的 [out] 参数包装代码"""
        base_type = param.type_info.base_type
        is_interface = self._is_interface_type(base_type)

        qualified_interface_name = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name

        lines = []

        if is_interface:
            lines.append(f"""
#ifdef SWIGJAVA
%rename("{method.name}_java_impl") {qualified_interface_name}::{method.name};
%javamethodmodifiers {qualified_interface_name}::{method.name} "private"
%typemap(javacode) {qualified_interface_name} %{{
    public {base_type} {method.name}(""")

            params = []
            for p in method.parameters:
                if p.direction != ParamDirection.OUT:
                    params.append(f"{self._get_java_type(p.type_info.base_type)} {p.name}")
            params_str = ", ".join(params)
            lines.append(f"{params_str}")

            lines.append(f""") throws DasException {{
        {self._get_java_type(base_type)}[] out_holder = new {self._get_java_type(base_type)}[1];
        DasResult hr = {method.name}_java_impl(""")

            param_names = []
            for p in method.parameters:
                if p.direction != ParamDirection.OUT:
                    param_names.append(p.name)
            param_names_str = ", ".join(param_names)
            lines.append(f"{param_names_str}")

            lines.append(f""", out_holder);
        if (hr < 0) {{
            throw new DasException(hr);
        }}
        return out_holder[0];
    }}
%}}
#endif
""")
        else:
            lines.append(f"""
#ifdef SWIGJAVA
%rename("{method.name}_java_impl") {qualified_interface_name}::{method.name};
%javamethodmodifiers {qualified_interface_name}::{method.name} "private"
%typemap(javacode) {qualified_interface_name} %{{
    public {self._get_java_type(base_type)} {method.name}(""")

            params = []
            for p in method.parameters:
                if p.direction != ParamDirection.OUT:
                    params.append(f"{self._get_java_type(p.type_info.base_type)} {p.name}")
            params_str = ", ".join(params)
            lines.append(f"{params_str}")

            lines.append(f""") throws DasException {{
        {self._get_java_type(base_type)}[] out_holder = new {self._get_java_type(base_type)}[1];
        DasResult hr = {method.name}_java_impl(""")

            param_names = []
            for p in method.parameters:
                if p.direction != ParamDirection.OUT:
                    param_names.append(p.name)
            param_names_str = ", ".join(param_names)
            lines.append(f"{param_names_str}")

            lines.append(f""", out_holder);
        if (hr < 0) {{
            throw new DasException(hr);
        }}
        return out_holder[0];
    }}
%}}
#endif
""")

        return "\n".join(lines)

    def generate_binary_buffer_helpers(self, interface: InterfaceDef, method_name: str, size_method_name: str) -> str:
        """生成 Java 的二进制缓冲区辅助方法"""
        qualified_name = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name
        native_name = f'{interface.name}_createDirectByteBuffer'

        return f"""
#ifdef SWIGJAVA
%typemap(javaclassmodifiers) {qualified_name} "public class"

%typemap(javacode) {qualified_name} %{{
    private static native java.nio.ByteBuffer {native_name}(long address, int capacity);

    public java.nio.ByteBuffer getDataAsDirectBuffer() {{
        long[] ptrHolder = new long[1];
        long[] sizeHolder = new long[1];

        int hr = GetDataNative(ptrHolder);
        if (hr < 0) {{
            throw new RuntimeException("Failed to get data pointer");
        }}

        hr = {size_method_name}Native(sizeHolder);
        if (hr < 0) {{
            throw new RuntimeException("Failed to get data size");
        }}

        return {native_name}(ptrHolder[0], (int)sizeHolder[0]);
    }}
%}}

%native({native_name}) jobject {native_name}(jlong address, jint capacity);
%{{
JNIEXPORT jobject JNICALL Java_{qualified_name.replace('::', '_')}_{native_name}(
    JNIEnv *jenv, jclass jcls, jlong address, jint capacity) {{
    return jenv->NewDirectByteBuffer((void*)address, capacity);
}}
%}}
#endif
"""
