"""
C# SWIG 生成器

生成 C# 特定的 SWIG .i 文件代码
"""

from das_idl_parser import InterfaceDef
from swig_lang_generator_base import SwigLangGenerator


class CSharpSwigGenerator(SwigLangGenerator):
    """C# 特定的 SWIG 代码生成器"""

    def get_language_name(self) -> str:
        return 'csharp'

    def get_swig_define(self) -> str:
        return 'SWIGCSHARP'

    def generate_out_param_wrapper(self, interface: InterfaceDef, method, param) -> str:
        """C# 不需要特殊的 [out] 参数包装代码，返回空字符串"""
        return ""

    def generate_binary_buffer_helpers(self, interface: InterfaceDef, method_name: str, size_method_name: str) -> str:
        """生成 C# 的二进制缓冲区辅助方法"""
        qualified_name = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name

        code = f"""
#ifdef SWIGCSHARP
%typemap(csclassmodifiers) {qualified_name} "public partial class"

%typemap(cscode) {qualified_name} %{{
    public System.IntPtr GetDataPointer() {{
        System.IntPtr ptr = System.IntPtr.Zero;
        var result = GetData(out ptr);
        if (result < 0) {{
            throw new System.Exception("Failed to get data pointer");
        }}
        return ptr;
    }}

    public unsafe System.Span<byte> GetDataAsSpan() {{
        var ptr = GetDataPointer();
        ulong size;
        {size_method_name}(out size);
        return new System.Span<byte>(ptr.ToPointer(), (int)size);
    }}

    public byte[] GetDataAsByteArray() {{
        var ptr = GetDataPointer();
        ulong size;
        {size_method_name}(out size);
        var array = new byte[size];
        System.Runtime.InteropServices.Marshal.Copy(ptr, array, 0, (int)size);
        return array;
    }}
%}}
#endif
"""
        return code
