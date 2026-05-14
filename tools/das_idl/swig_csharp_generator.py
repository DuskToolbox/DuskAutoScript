"""
C# SWIG 生成器

生成 C# 特定的 SWIG .i 文件代码
"""

from typing import List, Optional, TYPE_CHECKING
from das_idl_parser import InterfaceDef, MethodDef, ParameterDef, ParamDirection, TypeInfo, TypeKind
from swig_lang_generator_base import SwigLangGenerator
from shared_utils import build_param_signatures, type_simple_name, type_resolved_namespace

if TYPE_CHECKING:
    from swig_api_model import SwigInterfaceModel, OutParamInfo


class CSharpSwigGenerator(SwigLangGenerator):
    """C# 特定的 SWIG 代码生成器"""

    # C# bridge lifecycle method template — injected into every ISwig director class
    _BRIDGE_LIFECYCLE_CODE: str = """    // =========================================================================
    // Bridge lifecycle — prevent/release GC pinning for director objects
    // =========================================================================
    private System.Runtime.InteropServices.GCHandle? __dasBridgeHandle;

    private int __das_bridge_prevent() {
        if (!__dasBridgeHandle.HasValue) {
            __dasBridgeHandle = System.Runtime.InteropServices.GCHandle.Alloc(this);
        }
        return 0;
    }

    private int __das_bridge_release() {
        if (__dasBridgeHandle.HasValue) {
            __dasBridgeHandle.Value.Free();
            __dasBridgeHandle = null;
        }
        return 0;
    }"""

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

    @staticmethod
    def _is_binary_buffer_method(method: MethodDef) -> bool:
        return method.attributes.get('binary_buffer', False) if method.attributes else False

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

    def generate_pre_include_directives(self, interface: InterfaceDef) -> str:
        """生成必须在 %include 之前的 SWIG 指令
        
        将所有 %typemap(cscode) 生成逻辑移到此处，确保 typemap 在 SWIG 解析类之前声明。
        """
        # 如果 on_interface_model 还没被调用，返回空
        if self._pending_interface is None:
            return ""

        has_binary_buffer = any(self._is_binary_buffer_method(m) for m in interface.methods)

        for method in interface.methods:
            if self._is_binary_buffer_method(method):
                self._store_ignore_directive(
                    interface,
                    f"{method.name}_binary_buffer",
                    self._generate_ignore_directive_for_binary_buffer(interface, method),
                )

        for method, out_param in self._get_out_param_methods(interface):
            self._store_ignore_directive(
                interface,
                method.name,
                self._generate_ignore_directive(interface, method, out_param),
            )

        for method in self._get_methods_with_string_params_only(interface):
            self._store_ignore_directive(
                interface,
                f"{method.name}_string",
                self._generate_ignore_directive_for_string_method(interface, method),
            )

        qualified_name = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name

        all_methods = []

        # 1. Bridge lifecycle methods（为所有接口生成）
        all_methods.append(self._BRIDGE_LIFECYCLE_CODE)

        if has_binary_buffer:
            # Binary buffer 接口只需 lifecycle 方法，其他 helper 由
            # generate_binary_buffer_helpers() 单独生成
            methods_code = "\n\n".join(all_methods)
            return f'''
#ifdef SWIGCSHARP
%typemap(csclassmodifiers) {qualified_name} "public partial class"

%typemap(cscode) {qualified_name} %{{
{methods_code}
%}}
#endif
'''

        # 2. CastFrom/CreateFromPtr helper methods（为所有接口生成）
        helper_methods = self._generate_interface_helper_methods(interface)
        if helper_methods:
            all_methods.append(helper_methods)

        # 3. Ez methods
        for method_info in self._pending_out_methods:
            ez_code = self._generate_single_out_ez_method(method_info)
            all_methods.append(ez_code)

        for method_info in self._pending_multi_out_methods:
            ez_code = self._generate_multi_out_ez_method(method_info)
            all_methods.append(ez_code)

        # 4. String param helpers
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

    def _generate_interface_helper_methods(self, interface: InterfaceDef) -> str:
        interface_name = interface.name

        code = f'''    // =========================================================================
    // 接口辅助方法
    // CastFrom: 从 IDasBase 零开销转换到目标类型
    // CreateFromPtr: 内部工厂方法，供 as() 使用
    // =========================================================================
    
    /// <summary>
    /// 从 IDasBase 转换到 {interface_name}
    /// </summary>
    /// <param name="baseObj">源 IDasBase 对象</param>
    /// <returns>转换后的 {interface_name} 实例</returns>
    /// <exception cref="System.InvalidOperationException">当源对象为空或不拥有内存时抛出</exception>
    public static {interface_name} CastFrom(IDasBase baseObj) {{
        if (baseObj == null) {{
            throw new System.InvalidOperationException("Cannot cast from null IDasBase");
        }}
        if (!baseObj.IsOwnershipOwner()) {{
            throw new System.InvalidOperationException("Cannot cast: source does not own memory.");
        }}
        System.IntPtr ptr = IDasBase.getCPtr(baseObj).Handle;
        baseObj.ReleaseOwnership();
        return new {interface_name}(ptr, true);
    }}

    /// <summary>
    /// 从 IntPtr 创建 {interface_name} 实例（内部使用）
    /// </summary>
    /// <param name="cPtr">C++ 对象指针</param>
    /// <param name="cMemoryOwn">是否拥有内存所有权</param>
    /// <returns>新的 {interface_name} 实例</returns>
    public static {interface_name} CreateFromPtr(System.IntPtr cPtr, bool cMemoryOwn) {{
        return new {interface_name}(cPtr, cMemoryOwn);
    }}'''

        return code

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
            DasExceptionSourceInfoSwig sourceInfo = new DasExceptionSourceInfoSwig();
            sourceInfo.File = "{interface_name}.cs";
            sourceInfo.Line = 0;
            sourceInfo.Function = "{ez_method_name}";
            IDasExceptionString exStr = DuskAutoScript.CreateDasExceptionStringSwig(ret.GetErrorCode(), sourceInfo);
            throw new DasException(ret.GetErrorCode(), exStr);
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
            DasExceptionSourceInfoSwig sourceInfo = new DasExceptionSourceInfoSwig();
            sourceInfo.File = "{interface_name}.cs";
            sourceInfo.Line = 0;
            sourceInfo.Function = "{ez_method_name}";
            IDasExceptionString exStr = DuskAutoScript.CreateDasExceptionStringSwig(ret.GetErrorCode(), sourceInfo);
            throw new DasException(ret.GetErrorCode(), exStr);
        }}
        return ({getter_calls});
    }}'''
        
        return code

    def emit_post_include(self, model: "SwigInterfaceModel", interface_def: InterfaceDef) -> str:
        return ""

    def _store_ignore_directive(self, interface: InterfaceDef, key_suffix: str, ignore_code: str) -> None:
        if not self._context:
            return

        typemap_key = f"{interface.namespace}::{interface.name}::{key_suffix}"
        if typemap_key not in self._context._global_typemaps_ignore:
            self._context._global_typemaps_ignore[typemap_key] = ignore_code

    def _get_out_param_methods(self, interface: InterfaceDef) -> List[tuple[MethodDef, ParameterDef]]:
        result: List[tuple[MethodDef, ParameterDef]] = []

        for method in interface.methods:
            if self._is_binary_buffer_method(method):
                continue

            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            if len(out_params) == 1:
                result.append((method, out_params[0]))

        for prop in interface.properties:
            if not prop.has_getter:
                continue

            method_name = f"Get{prop.name}"
            if any(m.name == method_name for m in interface.methods):
                continue

            base_type = type_simple_name(prop.type_info)
            is_interface = prop.type_info.type_kind == TypeKind.INTERFACE
            is_string = base_type == 'IDasReadOnlyString'
            out_type = base_type if (is_interface or is_string) else base_type
            pointer_level = 2 if (is_interface or is_string) else 1

            out_param = ParameterDef(
                name='p_out',
                type_info=TypeInfo(
                    base_type=out_type,
                    is_pointer=True,
                    pointer_level=pointer_level,
                    is_const=False,
                    is_reference=False,
                ),
                direction=ParamDirection.OUT,
            )

            virtual_method = MethodDef(
                name=method_name,
                return_type=TypeInfo(base_type='DasResult'),
                parameters=[out_param],
                attributes={'_is_property': True, '_prop_name': prop.name},
            )
            result.append((virtual_method, out_param))

        return result

    def _get_methods_with_string_params_only(self, interface: InterfaceDef) -> List[MethodDef]:
        """获取接口中所有不带 [out] 参数但有 IDasReadOnlyString* 输入参数的方法"""
        result: List[MethodDef] = []
        for method in interface.methods:
            if self._is_binary_buffer_method(method):
                continue

            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            if out_params:
                continue
            has_string_param = any(
                p.direction == ParamDirection.IN and type_simple_name(p.type_info) == 'IDasReadOnlyString'
                for p in method.parameters
            )
            if has_string_param:
                result.append(method)

        for prop in interface.properties:
            if not prop.has_setter or type_simple_name(prop.type_info) != 'IDasReadOnlyString':
                continue

            method_name = f"Set{prop.name}"
            if any(m.name == method_name for m in interface.methods):
                continue

            setter_param = ParameterDef(
                name='p_value',
                type_info=TypeInfo(
                    base_type='IDasReadOnlyString',
                    is_pointer=True,
                    pointer_level=1,
                    is_const=False,
                    is_reference=False,
                ),
                direction=ParamDirection.IN,
            )

            virtual_method = MethodDef(
                name=method_name,
                return_type=TypeInfo(base_type='DasResult'),
                parameters=[setter_param],
                attributes={'_is_property': True, '_prop_name': prop.name},
            )
            result.append(virtual_method)

        return result

    def _generate_ignore_directive_for_binary_buffer(self, interface: InterfaceDef, method: MethodDef) -> str:
        qualified_interface = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name

        param_signatures_with_prefix, param_signatures_without_prefix = build_param_signatures(
            method, self.get_type_namespace, interface.namespace, TypeKind.INTERFACE,
        )

        param_list_with_prefix = ", ".join(param_signatures_with_prefix)
        param_list_without_prefix = ", ".join(param_signatures_without_prefix)

        result = f"""
#ifdef SWIGCSHARP
// 隐藏原始的 {method.name} 方法（[binary_buffer] 标记，C# 使用辅助方法，带 :: 前缀）
%ignore {qualified_interface}::{method.name}({param_list_with_prefix});
"""
        if param_list_with_prefix != param_list_without_prefix:
            result += f"""
// 隐藏原始的 {method.name} 方法（[binary_buffer] 标记，C# 使用辅助方法，不带 :: 前缀）
%ignore {qualified_interface}::{method.name}({param_list_without_prefix});
"""
        result += "\n#endif\n"
        return result

    def _generate_ignore_directive_for_string_method(self, interface: InterfaceDef, method: MethodDef) -> str:
        qualified_interface = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name

        param_signatures_with_prefix, param_signatures_without_prefix = build_param_signatures(
            method, self.get_type_namespace, interface.namespace, TypeKind.INTERFACE,
            include_const=True, include_reference=True,
        )

        param_list_with_prefix = ", ".join(param_signatures_with_prefix)
        param_list_without_prefix = ", ".join(param_signatures_without_prefix)

        result = f"""
// 隐藏原始的 {method.name} 方法（IDasReadOnlyString* 参数版本，带 :: 前缀）
%ignore {qualified_interface}::{method.name}({param_list_with_prefix});
"""
        if param_list_with_prefix != param_list_without_prefix:
            result += f"""
// 隐藏原始的 {method.name} 方法（IDasReadOnlyString* 参数版本，不带 :: 前缀）
%ignore {qualified_interface}::{method.name}({param_list_without_prefix});
"""
        return result

    def _generate_ignore_directive(self, interface: InterfaceDef, method: MethodDef, out_param: ParameterDef) -> str:
        qualified_interface = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name

        param_signatures_with_prefix = []
        param_signatures_without_prefix = []
        current_namespace = interface.namespace
        for param in method.parameters:
            param_type = type_simple_name(param.type_info)
            param_type_with_prefix = param_type
            namespace = type_resolved_namespace(param.type_info, self.get_type_namespace)
            if not namespace and param.type_info.type_kind == TypeKind.INTERFACE:
                param_type_with_prefix = f'::{param_type}'
            elif namespace and namespace != current_namespace:
                param_type_with_prefix = f'::{namespace}::{param_type}'

            is_out_param = param.direction == ParamDirection.OUT or param is out_param
            if param.type_info.is_pointer or (is_out_param and param.type_info.type_kind == TypeKind.INTERFACE):
                if is_out_param:
                    if param.type_info.type_kind == TypeKind.INTERFACE:
                        pointer_level = 2
                    else:
                        pointer_level = param.type_info.pointer_level if param.type_info.is_pointer else 1
                else:
                    pointer_level = param.type_info.pointer_level

                stars = '*' * pointer_level
                const_prefix = "const " if param.type_info.is_const else ""
                param_signatures_with_prefix.append(f"{const_prefix}{param_type_with_prefix}{stars}")
                param_signatures_without_prefix.append(f"{const_prefix}{param_type}{stars}")
            elif param.type_info.is_reference:
                if param.type_info.is_const:
                    param_signatures_with_prefix.append(f"const {param_type_with_prefix}&")
                    param_signatures_without_prefix.append(f"const {param_type}&")
                else:
                    param_signatures_with_prefix.append(f"{param_type_with_prefix}&")
                    param_signatures_without_prefix.append(f"{param_type}&")
            else:
                param_signatures_with_prefix.append(param_type_with_prefix)
                param_signatures_without_prefix.append(param_type)

        param_list_with_prefix = ", ".join(param_signatures_with_prefix)
        param_list_without_prefix = ", ".join(param_signatures_without_prefix)

        result = f"""
// 隐藏原始的 {method.name} 方法（带完整参数签名，带命名空间前缀）
%ignore {qualified_interface}::{method.name}({param_list_with_prefix});
"""
        if param_list_with_prefix != param_list_without_prefix:
            result += f"""
// 隐藏原始的 {method.name} 方法（不带命名空间前缀）
%ignore {qualified_interface}::{method.name}({param_list_without_prefix});
"""

        if any(
            p.direction == ParamDirection.IN and type_simple_name(p.type_info) == 'IDasReadOnlyString'
            for p in method.parameters
        ):
            extend_param_signatures = []
            for param in method.parameters:
                if param.direction == ParamDirection.OUT:
                    continue

                param_type = type_simple_name(param.type_info)
                if param.type_info.type_kind == TypeKind.INTERFACE and param_type != 'IDasReadOnlyString':
                    namespace = type_resolved_namespace(param.type_info, self.get_type_namespace)
                    if namespace:
                        param_type = f'{namespace}::{param_type}'

                if param.type_info.is_pointer:
                    stars = '*' * param.type_info.pointer_level
                    extend_param_signatures.append(f"{param_type}{stars}")
                else:
                    extend_param_signatures.append(param_type)

            extend_param_list = ", ".join(extend_param_signatures)
            result += f"""
// 隐藏 string helper 覆盖前的原始 {method.name} 方法（保留 string 版本）
%ignore {qualified_interface}::{method.name}({extend_param_list});
"""

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
        return_type = type_simple_name(method.return_type) if method.return_type else "DasResult"
        interface_name = self._pending_interface_name
        
        # 构建参数列表
        cs_params = []
        call_args = []
        
        for p in method.parameters:
            param_name = p.name
            if type_simple_name(p.type_info) == 'IDasReadOnlyString':
                cs_params.append(f"string {param_name}")
                call_args.append(f"new DasReadOnlyString({param_name})")
            else:
                cs_type = self._get_cs_type(type_simple_name(p.type_info))
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
        type_name = type_name.split('::')[-1]
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

