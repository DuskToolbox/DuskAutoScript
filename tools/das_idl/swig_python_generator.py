"""
Python SWIG 生成器

生成 Python 特定的 SWIG .i 文件代码
"""

from das_idl_parser import InterfaceDef
from swig_lang_generator_base import SwigLangGenerator


class PythonSwigGenerator(SwigLangGenerator):
    """Python 特定的 SWIG 代码生成器"""

    def get_language_name(self) -> str:
        return 'python'

    def get_swig_define(self) -> str:
        return 'SWIGPYTHON'

    def generate_out_param_wrapper(self, interface: InterfaceDef, method, param) -> str:
        """Python 不需要特殊的 [out] 参数包装代码，返回空字符串"""
        return ""

    def generate_binary_buffer_helpers(self, interface: InterfaceDef, method_name: str, size_method_name: str) -> dict:
        """生成 Python 的二进制缓冲区辅助方法"""
        qualified_name = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name

        code = f"""
#ifdef SWIGPYTHON
%extend {qualified_name} {{
    PyObject* GetDataAsMemoryView() {{
        unsigned char* data = nullptr;
        uint64_t size = 0;

        DasResult hr = $self->{method_name}(&data);
        if (hr < 0) {{
            PyErr_SetString(PyExc_RuntimeError, "Failed to get data pointer");
            return nullptr;
        }}

        hr = $self->{size_method_name}(&size);
        if (hr < 0) {{
            PyErr_SetString(PyExc_RuntimeError, "Failed to get data size");
            return nullptr;
        }}

        return PyMemoryView_FromMemory(reinterpret_cast<char*>(data), static_cast<Py_ssize_t>(size), PyBUF_READ);
    }}

    PyObject* GetDataAsBytes() {{
        unsigned char* data = nullptr;
        uint64_t size = 0;

        DasResult hr = $self->{method_name}(&data);
        if (hr < 0) {{
            PyErr_SetString(PyExc_RuntimeError, "Failed to get data pointer");
            return nullptr;
        }}

        hr = $self->{size_method_name}(&size);
        if (hr < 0) {{
            PyErr_SetString(PyExc_RuntimeError, "Failed to get data size");
            return nullptr;
        }}

        return PyBytes_FromStringAndSize(reinterpret_cast<const char*>(data), static_cast<Py_ssize_t>(size));
    }}
}}
#endif
"""
        return {
            'typemaps': [],
            'code': code
        }
