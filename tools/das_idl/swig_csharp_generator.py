"""
C# SWIG 生成器

生成 C# 特定的 SWIG .i 文件代码
"""

from typing import List, Optional, Tuple, TYPE_CHECKING
from das_idl_parser import InterfaceDef, MethodDef, ParameterDef, ParamDirection
from swig_lang_generator_base import SwigLangGenerator

if TYPE_CHECKING:
    from swig_api_model import SwigInterfaceModel, OutParamInfo


class CSharpSwigGenerator(SwigLangGenerator):
    """C# 特定的 SWIG 代码生成器"""

    def __init__(self) -> None:
        super().__init__()
        self._pending_out_methods: List["OutParamInfo"] = []
        self._pending_multi_out_methods: List["OutParamInfo"] = []
        self._pending_interface_name: str = ""
        self._pending_interface: Optional[InterfaceDef] = None
        self._pending_string_methods: List[MethodDef] = []

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
// 将 GetData 重命名为内部方法，并设为私有
// 类似于 Java 端的处理方式
%rename("GetData_internal") {qualified_name}::GetData;
%csmethodmodifiers {qualified_name}::GetData "private";

// 禁用 GetData 的 Director 功能
%feature("nodirector") {qualified_name}::GetData;

// GetData 的 [out] 参数转换为 System.IntPtr
// ctype: C/C++ 端类型
// imtype: P/Invoke 中间类型
// cstype: C# 公开类型
// csin: C# 调用时的参数转换
%typemap(ctype) unsigned char** pp_out_data "unsigned char**"
%typemap(imtype) unsigned char** pp_out_data "out System.IntPtr"
%typemap(cstype) unsigned char** pp_out_data "out System.IntPtr"
%typemap(csin) unsigned char** pp_out_data "out $csinput"
%typemap(in) unsigned char** pp_out_data (unsigned char* temp = nullptr) {{
    $1 = &temp;
}}
%typemap(freearg) unsigned char** pp_out_data ""  // 不释放临时变量
%typemap(argout) unsigned char** pp_out_data ""  // argout 不需要处理，SWIG 会自动传递指针值
// 注意：不使用 csargout，让 SWIG 自动处理 out 参数
// 注入 C# 便捷方法 - 高效零拷贝内存访问
%typemap(cscode) {qualified_name} %{{
    /// <summary>
    /// 获取数据指针（零拷贝）
    /// </summary>
    public System.IntPtr GetDataPointer() {{
        System.IntPtr ptr = System.IntPtr.Zero;
        var result = GetData_internal(out ptr);
        if (result < 0) {{
            throw new System.Exception("Failed to get data pointer");
        }}
        return ptr;
    }}

    /// <summary>
    /// 获取数据大小
    /// </summary>
    public ulong GetDataSize() {{
        ulong size;
        {size_method_name}(out size);
        return size;
    }}

    /// <summary>
    /// 获取数据作为 Span（零拷贝，高性能）
    /// 警告：Span 生命周期与底层内存绑定，请勿在释放内存后使用
    /// </summary>
    public unsafe System.Span<byte> GetDataAsSpan() {{
        var ptr = GetDataPointer();
        var size = GetDataSize();
        return new System.Span<byte>(ptr.ToPointer(), (int)size);
    }}

    /// <summary>
    /// 获取数据作为只读 Span（零拷贝，高性能）
    /// </summary>
    public unsafe System.ReadOnlySpan<byte> GetDataAsReadOnlySpan() {{
        var ptr = GetDataPointer();
        var size = GetDataSize();
        return new System.ReadOnlySpan<byte>(ptr.ToPointer(), (int)size);
    }}

    /// <summary>
    /// 直接拷贝到目标缓冲区（零中间分配，高性能）
    /// </summary>
    /// <param name="destination">目标缓冲区</param>
    /// <returns>实际拷贝的字节数</returns>
    public int CopyTo(System.Span<byte> destination) {{
        var source = GetDataAsSpan();
        var copyLength = System.Math.Min(source.Length, destination.Length);
        source.Slice(0, copyLength).CopyTo(destination);
        return copyLength;
    }}

    /// <summary>
    /// 异步拷贝到目标流（高性能）
    /// </summary>
    public async System.Threading.Tasks.Task CopyToAsync(System.IO.Stream destination) {{
        var span = GetDataAsSpan();
        await destination.WriteAsync(span).ConfigureAwait(false);
    }}

    /// <summary>
    /// 获取数据副本（分配新数组，用于需要独立所有权的场景）
    /// 注意：此方法会分配新内存并拷贝数据
    /// </summary>
    }}
%}}
#endif
"""
        return code

    def on_interface_model(self, model: "SwigInterfaceModel", interface_def: InterfaceDef) -> None:
        """处理接口分析模型
        
        收集 out 方法信息和字符串参数方法信息，供 emit_post_include 使用
        
        Args:
            model: 接口分析模型
            interface_def: 原始接口定义
        """
        self._pending_interface = interface_def
        self._pending_interface_name = interface_def.name
        self._pending_out_methods = list(model.out_methods)
        self._pending_multi_out_methods = list(model.multi_out_methods)
        self._pending_string_methods = self._get_methods_with_string_params_only(interface_def)

    def _generate_single_out_ez_method(self, method_info: "OutParamInfo") -> str:
        """生成单 out 参数的 Ez 方法
        
        Args:
            method_info: Out 参数方法信息
            
        Returns:
            C# 方法代码字符串
        """
        method_name = method_info.method_name
        ez_method_name = f"{method_name}Ez"
        out_type = method_info.out_param_type
        interface_name = self._pending_interface_name
        
        in_params_str = ", ".join([f"{ptype} {pname}" for ptype, pname in method_info.in_params])
        in_args_str = ", ".join([pname for _, pname in method_info.in_params])
        
        if out_type == "void":
            return_type = "IDasBase"
            getter_call = "ret.GetValue()"
        else:
            return_type = out_type
            if out_type in ["IDasOcrResult", "IDasTaskInfo", "IDasGuid", "IDasBase", "IDasTypeInfo"]:
                getter_call = "ret.GetValue()"
            elif out_type in ["int32", "int64", "uint32", "uint64", "float", "double", "bool", "DasGuid"]:
                getter_call = "ret.GetValue()"
            elif out_type == "size_t":
                getter_call = "ret.GetCount()"
            else:
                getter_call = "ret.GetValue()"
        
        if in_params_str:
            signature = f"public {return_type} {ez_method_name}({in_params_str})"
        else:
            signature = f"public {return_type} {ez_method_name}()"
        
        code = f'''    {signature} {{
        var ret = {method_name}({in_args_str});
        if (!ret.IsOk()) {{
            throw new System.Exception($"[{{ret.GetErrorCode()}}] {interface_name}.cs:0 {ez_method_name}");
        }}
        return {getter_call};
    }}'''
        
        return code

    def _generate_multi_out_ez_method(self, method_info: "OutParamInfo") -> str:
        """生成多 out 参数的 Ez 方法
        
        多 out 返回 C# tuple
        
        Args:
            method_info: Out 参数方法信息
            
        Returns:
            C# 方法代码字符串
        """
        method_name = method_info.method_name
        ez_method_name = f"{method_name}Ez"
        interface_name = self._pending_interface_name
        
        # 构建输入参数列表
        in_params_str = ", ".join([f"{ptype} {pname}" for ptype, pname in method_info.in_params])
        in_args_str = ", ".join([pname for _, pname in method_info.in_params])
        
        if method_info.all_out_params:
            return_types = ", ".join(method_info.all_out_params)
        else:
            return_types = f"{method_info.out_param_type}, ..."
        
        if method_info.all_out_params and len(method_info.all_out_params) == 2:
            getter_calls = "ret.GetResult(), ret.GetResultCount()"
        else:
            getter_calls = "ret.GetResult(), ret.GetResultCount()"
        
        if in_params_str:
            signature = f"public ({return_types}) {ez_method_name}({in_params_str})"
        else:
            signature = f"public ({return_types}) {ez_method_name}()"
        
        code = f'''    {signature} {{
        var ret = {method_name}({in_args_str});
        if (!ret.IsOk()) {{
            throw new System.Exception($"[{{ret.GetErrorCode()}}] {interface_name}.cs:0 {ez_method_name}");
        }}
        return ({getter_calls});
    }}'''
        
        return code

    def emit_post_include(self, model: "SwigInterfaceModel", interface_def: InterfaceDef) -> str:
        """生成后置包含指令（Ez方法）
        
        使用 %typemap(cscode) 注入 C# 代码到 proxy 类
        
        Args:
            model: 接口分析模型
            interface_def: 原始接口定义
            
        Returns:
            SWIG .i 文件代码片段
        """
        qualified_name = model.qualified_name
        
        all_out_methods = self._pending_out_methods + self._pending_multi_out_methods
        
        if not all_out_methods and not self._pending_string_methods:
            return ""
        
        all_methods = []
        
        for method_info in self._pending_out_methods:
            ez_code = self._generate_single_out_ez_method(method_info)
            all_methods.append(ez_code)
        
        for method_info in self._pending_multi_out_methods:
            ez_code = self._generate_multi_out_ez_method(method_info)
            all_methods.append(ez_code)
        
        for method in self._pending_string_methods:
            helper_code = self._generate_single_string_param_helper(method, qualified_name)
            if helper_code:
                all_methods.append(helper_code)
        
        methods_code = "\n\n".join(all_methods)
        
        return f'''
#ifdef SWIGCSHARP
%typemap(csclassmodifiers) {qualified_name} "public partial class"

%typemap(cscode) {qualified_name} %{{
{methods_code}
%}}
#endif
'''

    def _get_methods_with_string_params_only(self, interface: InterfaceDef) -> List[MethodDef]:
        """获取接口中所有不带 [out] 参数但有 IDasReadOnlyString* 输入参数的方法"""
        result: List[MethodDef] = []
        for method in interface.methods:
            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            if out_params:
                continue
            has_string_param = any(
                p.direction == ParamDirection.IN and p.type_info.base_type == 'IDasReadOnlyString'
                for p in method.parameters
            )
            if has_string_param:
                result.append(method)
        return result

    def _generate_string_param_helpers(self, interface: InterfaceDef) -> str:
        """生成字符串参数便捷方法"""
        if not self._pending_string_methods:
            return ""
        
        qualified_name = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name
        helpers = []
        
        for method in self._pending_string_methods:
            helper_code = self._generate_single_string_param_helper(method, qualified_name)
            if helper_code:
                helpers.append(helper_code)
        
        if not helpers:
            return ""
        
        helpers_code = "\n\n".join(helpers)
        return f'''
#ifdef SWIGCSHARP
%typemap(csclassmodifiers) {qualified_name} "public partial class"

%typemap(cscode) {qualified_name} %{{
{helpers_code}
%}}
#endif
'''

    def _generate_single_string_param_helper(self, method: MethodDef, qualified_name: str) -> str:
        """生成单个字符串参数便捷方法"""
        method_name = method.name
        return_type = method.return_type.base_type if method.return_type else "DasResult"
        interface_name = self._pending_interface_name
        
        # 构建参数列表
        cs_params = []
        call_args = []
        
        for p in method.parameters:
            param_name = p.name
            if p.type_info.base_type == 'IDasReadOnlyString':
                cs_params.append(f"string {param_name}")
                call_args.append(f"new DasReadOnlyString({param_name})")
            else:
                cs_type = self._get_cs_type(p.type_info.base_type)
                cs_params.append(f"{cs_type} {param_name}")
                call_args.append(param_name)
        
        cs_params_str = ", ".join(cs_params)
        call_args_str = ", ".join(call_args)
        
        if cs_params_str:
            signature = f"public {return_type} {method_name}({cs_params_str})"
        else:
            signature = f"public {return_type} {method_name}()"
        
        code = f'''    {signature} {{
        return {method_name}({call_args_str});
    }}'''
        
        return code

    def _get_cs_type(self, type_name: str) -> str:
        """获取 C# 类型名称"""
        type_map = {
            'bool': 'bool',
            'int8': 'sbyte',
            'int16': 'short',
            'int32': 'int',
            'int64': 'long',
            'int64_t': 'long',
            'uint8': 'byte',
            'uint16': 'ushort',
            'uint32': 'uint',
            'uint64': 'ulong',
            'uint64_t': 'ulong',
            'float': 'float',
            'double': 'double',
            'size_t': 'ulong',
            'DasResult': 'DasResult',
            'DasGuid': 'DasGuid',
        }
        return type_map.get(type_name, type_name)

