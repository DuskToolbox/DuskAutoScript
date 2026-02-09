"""
Python SWIG 生成器

生成 Python 特定的 SWIG .i 文件代码
"""

from typing import TYPE_CHECKING, List
from das_idl_parser import InterfaceDef
from swig_lang_generator_base import SwigLangGenerator

if TYPE_CHECKING:
    from swig_api_model import SwigInterfaceModel, OutParamInfo


class PythonSwigGenerator(SwigLangGenerator):
    """Python 特定的 SWIG 代码生成器"""

    def __init__(self) -> None:
        super().__init__()
        self._pending_out_methods: List["OutParamInfo"] = []
        self._pending_multi_out_methods: List["OutParamInfo"] = []
        self._pending_interface_name: str = ""
        self._pending_interface: InterfaceDef = None  # type: ignore

    def get_language_name(self) -> str:
        return 'python'

    def get_swig_define(self) -> str:
        return 'SWIGPYTHON'

    def on_interface_model(self, model: "SwigInterfaceModel", interface_def: InterfaceDef) -> None:
        self._pending_out_methods = list(model.out_methods)
        self._pending_multi_out_methods = list(model.multi_out_methods)
        self._pending_interface_name = model.name
        self._pending_interface = interface_def

    def emit_post_include(self, model: "SwigInterfaceModel", interface_def: InterfaceDef) -> str:
        out_methods = getattr(self, '_pending_out_methods', [])
        multi_out_methods = getattr(self, '_pending_multi_out_methods', [])
        interface_name = getattr(self, '_pending_interface_name', model.name)

        if not out_methods and not multi_out_methods:
            return ""

        ez_methods = []

        for method_info in out_methods:
            ez_code = self._generate_single_out_ez_method(method_info, interface_name)
            if ez_code:
                ez_methods.append(ez_code)

        for method_info in multi_out_methods:
            ez_code = self._generate_multi_out_ez_method(method_info, interface_name)
            if ez_code:
                ez_methods.append(ez_code)

        if not ez_methods:
            return ""

        methods_code = "\n\n".join(ez_methods)
        return f'''#ifdef SWIGPYTHON
%pythoncode %{{
{methods_code}
%}}
#endif
'''

    def _generate_single_out_ez_method(self, method_info: "OutParamInfo", interface_name: str) -> str:
        method_name = method_info.method_name
        out_type = method_info.out_param_type
        in_params = method_info.in_params
        is_void_pointer = out_type == 'void'

        param_list = []
        call_args = []
        for param_type, param_name in in_params:
            param_list.append(param_name)
            call_args.append(param_name)

        params_str = ", ".join(param_list) if param_list else ""
        call_args_str = ", ".join(call_args) if call_args else ""

        method_signature = f"def {method_name}Ez(self, {params_str}):" if params_str else f"def {method_name}Ez(self):"

        if is_void_pointer:
            return f'''    {method_signature}
        ret = self.{method_name}({call_args_str})
        if not ret.isOk():
            raise RuntimeError(f"[{{ret.getErrorCode()}}] {interface_name}.py:0 {method_name}Ez")
        return ret.getValue()'''
        else:
            return f'''    {method_signature}
        ret = self.{method_name}({call_args_str})
        if not ret.isOk():
            raise RuntimeError(f"[{{ret.getErrorCode()}}] {interface_name}.py:0 {method_name}Ez")
        return ret.getValue()'''

    def _generate_multi_out_ez_method(self, method_info: "OutParamInfo", interface_name: str) -> str:
        method_name = method_info.method_name
        in_params = method_info.in_params

        param_list = []
        call_args = []
        for param_type, param_name in in_params:
            param_list.append(param_name)
            call_args.append(param_name)

        params_str = ", ".join(param_list) if param_list else ""
        call_args_str = ", ".join(call_args) if call_args else ""

        method_signature = f"def {method_name}Ez(self, {params_str}):" if params_str else f"def {method_name}Ez(self):"

        return f'''    {method_signature}
        ret = self.{method_name}({call_args_str})
        if not ret.isOk():
            raise RuntimeError(f"[{{ret.getErrorCode()}}] {interface_name}.py:0 {method_name}Ez")
        return ret.getValues()'''

    def generate_out_param_wrapper(self, interface: InterfaceDef, method, param) -> str:
        """Python 不需要特殊的 [out] 参数包装代码，返回空字符串"""
        return ""

    def generate_binary_buffer_helpers(self, interface: InterfaceDef, method_name: str, size_method_name: str) -> str:
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
        return code
