"""
Lua SWIG 生成器

生成 Lua 特定的 SWIG .i 文件代码
"""

import re
from typing import TYPE_CHECKING, List
from das_idl_parser import (
    IdlDocument,
    ErrorCodeDef,
    ErrorCodeValue,
    ModuleDef,
    ModuleFunctionDef,
    InterfaceDef,
    MethodDef,
    ParameterDef,
    ParamDirection,
    TypeInfo,
    TypeKind,
)
from swig_lang_generator_base import SwigLangGenerator

if TYPE_CHECKING:
    from swig_api_model import SwigInterfaceModel


class LuaSwigGenerator(SwigLangGenerator):
    """Lua 特定的 SWIG 代码生成器"""

    def get_language_name(self) -> str:
        return 'lua'

    def get_swig_define(self) -> str:
        return 'SWIGLUA'

    def generate_out_param_wrapper(self, interface: InterfaceDef, method, param) -> str:
        """Lua 不需要特殊的 [out] 参数包装代码，返回空字符串"""
        return ""

    def generate_binary_buffer_helpers(self, interface: InterfaceDef, method_name: str, size_method_name: str) -> str:
        """Lua 不需要特殊的二进制缓冲区辅助方法，返回空字符串"""
        return ""

    def _get_sol2_param_type(self, type_info: TypeInfo) -> str:
        """将 IDL TypeInfo 映射为 sol2 绑定中使用的 C++ 参数类型。

        映射规则:
          - DasGuid (BASIC) → const DasGuid&（内置原始类型，使用 const 引用传递）
          - INTERFACE → TypeName*（接口使用指针传递）
          - BASIC → 直接使用 base_type（int32_t, float, DasBool, DasString 等）
          - ENUM → 直接使用 base_type
          - STRUCT → 直接使用 base_type
          - UNKNOWN → 直接使用 base_type（兜底）

        在基础类型确定后，还会根据 is_const / is_reference / is_pointer 标志
        追加 const 修饰、引用 & 或指针 *。

        Args:
            type_info: 经 resolve_types() 标注后的类型信息。

        Returns:
            适合用于 sol2 函数参数声明的 C++ 类型字符串。
        """
        # DasGuid 特殊处理：按值传递时使用 const 引用；带指针时走正常路径
        if type_info.type_kind == TypeKind.BASIC and type_info.base_type == 'DasGuid':
            if not type_info.is_pointer:
                return "const DasGuid&"

        # 根据类型分类确定基础类型名（不加指针，统一由 pointer_level 控制）
        result = type_info.base_type

        # 根据 const / pointer / reference 标志追加修饰
        if type_info.is_const:
            result = f"const {result}"
        if type_info.is_pointer:
            result = f"{result}{'*' * type_info.pointer_level}"
        if type_info.is_reference:
            result = f"{result}&"

        return result

    def _get_sol2_return_type(self, type_info: TypeInfo) -> str:
        """将 IDL TypeInfo 映射为 sol2 绑定中使用的 C++ 返回类型。

        映射规则:
          - void → "void"
          - DasResult → "DasResult"（直接返回，非引用）
          - DasGuid → "const DasGuid&"（使用 const 引用）
          - INTERFACE → TypeName*（接口返回指针）
          - BASIC → 直接使用 base_type
          - ENUM → 直接使用 base_type
          - STRUCT → 直接使用 base_type

        Args:
            type_info: 经 resolve_types() 标注后的返回类型信息。

        Returns:
            适合用于 sol2 函数返回类型声明的 C++ 类型字符串。
        """
        # void 特殊处理
        if type_info.base_type == 'void':
            return "void"

        # DasResult 特殊处理：直接返回值类型
        if type_info.base_type == 'DasResult':
            return "DasResult"

        # DasGuid 特殊处理：按 const 引用返回
        if type_info.type_kind == TypeKind.BASIC and type_info.base_type == 'DasGuid':
            return "const DasGuid&"

        # 根据类型分类确定返回类型
        if type_info.type_kind == TypeKind.INTERFACE:
            # 接口返回指针: TypeName* (pointer_level=1 是接口指针的标准返回)
            if type_info.is_pointer and type_info.pointer_level > 0:
                return f"{type_info.base_type}{'*' * type_info.pointer_level}"
            return f"{type_info.base_type}*"

        # BASIC, ENUM, STRUCT, UNKNOWN → 直接使用 base_type
        return type_info.base_type

    def _generate_lua_director_base(self) -> str:
        """生成 LuaDirector 基类的 C++ 代码。

        LuaDirector 是所有 Director 包装类的基类，提供：
          - Lua 环境（lua_State*）和 registry 引用的存储与生命周期管理
          - upcall 检测机制，防止 C++→Lua→C++ 重入调用
          - UpcallGuard RAII 辅助类，用于作用域化的 upcall 保护
          - has_lua_method() 和 call_lua_method() 模板方法用于安全的 Lua 回调

        生成的代码依赖 sol2（#include <sol/sol.hpp>）和标准 Lua C API。
        #include 指令由主生成入口点负责输出，此方法不包含任何 #include。

        Returns:
            LuaDirector 类的完整 C++ 定义字符串。
        """
        return """\
// LuaDirector — base class for all Director wrapper classes
// Provides Lua environment storage, upcall detection, and method dispatch
class LuaDirector {
protected:
    lua_State* L_{nullptr};
    int env_ref_{LUA_NOREF};        // Lua registry reference to environment table
    bool upcall_active_{false};     // Prevents C++→Lua→C++ reentrant calls

public:
    LuaDirector() = default;

    virtual ~LuaDirector() {
        if (L_ != nullptr && env_ref_ != LUA_NOREF) {
            luaL_unref(L_, LUA_REGISTRYINDEX, env_ref_);
        }
    }

    void set_lua_environment(lua_State* L, int env_ref) {
        L_ = L;
        env_ref_ = env_ref;
    }

    bool is_upcall_active() const { return upcall_active_; }

    void set_upcall_active(bool active) { upcall_active_ = active; }

    // RAII guard for upcall detection
    class UpcallGuard {
        LuaDirector* director_;
    public:
        explicit UpcallGuard(LuaDirector* d) : director_(d) {
            director_->set_upcall_active(true);
        }
        ~UpcallGuard() {
            director_->set_upcall_active(false);
        }
    };

    bool has_lua_method(const char* name) const {
        if (L_ == nullptr || env_ref_ == LUA_NOREF) return false;
        sol::table env(L_, sol::ref_index(env_ref_));
        sol::object method = env[name];
        return method.valid() && method.is<sol::function>();
    }

    template<typename... Args>
    sol::protected_function_result call_lua_method(const char* name, Args&&... args) {
        sol::table env(L_, sol::ref_index(env_ref_));
        sol::object method_obj = env[name];
        if (!method_obj.valid() || !method_obj.is<sol::function>()) {
            return sol::protected_function_result();
        }
        sol::protected_function func = method_obj.as<sol::protected_function>();
        return func(std::forward<Args>(args)...);
    }
};
"""

    def _get_director_class_name(self, iface_name: str) -> str:
        """根据接口名推导 Director 类名。

        命名规则:
          - IDasLogger → ILuaDasLogger（以 IDas 开头）
          - ILogger   → ILuaLogger（以 I 开头，非 IDas）
          - Foo       → ILuaFoo（其他情况）

        Args:
            iface_name: 接口名称。

        Returns:
            Director 类名。
        """
        if iface_name.startswith('IDas'):
            return f'ILuaDas{iface_name[4:]}'
        elif iface_name.startswith('I'):
            return f'ILua{iface_name[1:]}'
        else:
            return f'ILua{iface_name}'

    @staticmethod
    def _uuid_to_das_guid_literal(uuid_str: str) -> str:
        """将 IDL UUID 字符串转换为 DasGuid 字面量。

        输入格式: "69A9BDB0-4657-45B6-8ECB-E4A8F0428E95"
        输出格式: "DasGuid{0x69A9BDB0, 0x4657, 0x45B6, {0x8E, 0xCB, 0xE4, 0xA8, 0xF0, 0x42, 0x8E, 0x95}}"

        Args:
            uuid_str: IDL 中的 UUID 字符串（含连字符）。

        Returns:
            DasGuid 初始化列表字符串。
        """
        hex_str = uuid_str.replace('-', '')
        # UUID layout: Data1(4B) Data2(2B) Data3(2B) Data4(8B)
        d1 = int(hex_str[0:8], 16)
        d2 = int(hex_str[8:12], 16)
        d3 = int(hex_str[12:16], 16)
        d4 = [int(hex_str[16 + i * 2: 16 + i * 2 + 2], 16) for i in range(8)]
        return (
            f"DasGuid{{0x{d1:08X}, 0x{d2:04X}, 0x{d3:04X},"
            f" {{0x{d4[0]:02X}, 0x{d4[1]:02X}, 0x{d4[2]:02X}, 0x{d4[3]:02X},"
            f" 0x{d4[4]:02X}, 0x{d4[5]:02X}, 0x{d4[6]:02X}, 0x{d4[7]:02X}}}}}"
        )

    def _generate_director_class(
        self,
        interface: InterfaceDef,
        all_interfaces: dict = None,
    ) -> str:
        """生成 ILuaDas{Name} Director 包装类的 C++ 定义。

        每个 Director 类同时继承 C++ 接口和 LuaDirector，
        提供 upcall 分发并带有回退到父接口默认实现的机制。

        命名规则:
          - IDasLogger → ILuaDasLogger（以 IDas 开头）
          - ILogger   → ILuaLogger（其他情况）

        Args:
            interface: 经 resolve_types() 标注后的接口定义。
            all_interfaces: 接口名 → InterfaceDef 映射（用于收集继承链上的方法）。

        Returns:
            完整的 Director 类 C++ 定义字符串。
        """
        iface_name = interface.name
        class_name = self._get_director_class_name(iface_name)

        parent = iface_name

        lines = []
        lines.append(f'class {class_name} : public {parent}, public LuaDirector {{')
        lines.append('public:')
        lines.append(f'    explicit {class_name}(sol::this_state ts, sol::table env)')
        lines.append('        : LuaDirector()')
        lines.append('    {')
        lines.append(
            '        lua_State* L = ts;'
        )
        lines.append(
            '        lua_pushvalue(L, -1);'
            '  // Push copy of the table for registry ref'
        )
        lines.append('        int env_ref = luaL_ref(L, LUA_REGISTRYINDEX);')
        lines.append('        set_lua_environment(L, env_ref);')
        lines.append('    }')

        # COM lifecycle (Atomic ref counting + thread-safe)
        lines.append('')
        lines.append('    std::atomic<uint32_t> ref_count_{1};')
        lines.append('')
        lines.append('    uint32_t AddRef() override {')
        lines.append(
            '        return ref_count_.fetch_add(1,'
            ' std::memory_order_relaxed) + 1;'
        )
        lines.append('    }')
        lines.append('')
        lines.append('    uint32_t Release() override {')
        lines.append(
            '            uint32_t count = ref_count_.fetch_sub(1,'
            ' std::memory_order_acq_rel) - 1;'
        )
        lines.append('            if (count == 0) {')
        lines.append('                delete this;')
        lines.append('            }')
        lines.append('            return count;')
        lines.append('    }')
        lines.append('')
        lines.append(
            '        DasResult QueryInterface(const DasGuid& iid,'
            ' void** pp_object) override {'
        )
        lines.append(
            '            if (!pp_object) return DAS_E_INVALID_POINTER;'
        )
        # Generate DasGuid literal for this interface's UUID
        iid_literal = self._uuid_to_das_guid_literal(interface.uuid)
        lines.append(
            f'            static const DasGuid IID_MyType = {iid_literal};'
        )
        lines.append(
            '            if (iid == DAS_IID_BASE || iid == IID_MyType) {'
        )
        lines.append(
            f'                *pp_object'
            f' = static_cast<{parent}*>(this);'
        )
        lines.append('                AddRef();')
        lines.append('                return DAS_S_OK;')
        lines.append('            }')
        lines.append('            *pp_object = nullptr;')
        lines.append('            return DAS_E_NO_INTERFACE;')
        lines.append('        }')

        # 收集本接口及所有基接口的方法（递归到 IDasBase 为止）
        own_method_names = {m.name for m in interface.methods}
        inherited_methods = []
        if all_interfaces:
            base_name = interface.base_interface
            while base_name and base_name != 'IDasBase':
                base_iface = all_interfaces.get(base_name)
                if not base_iface:
                    break
                for m in base_iface.methods:
                    if m.name not in own_method_names:
                        inherited_methods.append((m, base_name))
                        own_method_names.add(m.name)
                base_name = base_iface.base_interface

        # 为本接口的方法生成 override
        for method in interface.methods:
            lines.append(
                self._generate_method_override(method, parent)
            )

        # 为继承的基接口方法生成 override（确保所有纯虚函数都被实现）
        for method, base_name in inherited_methods:
            lines.append(
                self._generate_method_override(method, parent)
            )

        lines.append('};')
        return '\n'.join(lines)

    def _generate_method_override(
        self, method: MethodDef, parent: str
    ) -> str:
        """为接口的单个虚方法生成 Director override 实现。

        生成的 override 方法遵循以下模式:
          1. 检测 upcall 重入 → 回退到父接口
          2. 检测 Lua 方法是否存在 → 不存在则回退到父接口
          3. 使用 UpcallGuard 保护后调用 Lua 方法
          4. Lua 调用失败时根据返回类型提供适当的错误回退值

        Args:
            method: 方法定义（MethodDef）。
            parent: 父接口类名（用于回退调用）。

        Returns:
            方法 override 的完整 C++ 定义字符串。
        """
        ret_type = self._get_sol2_return_type(method.return_type)
        is_void = ret_type == 'void'
        is_das_result = ret_type == 'DasResult'
        is_interface_ptr = method.return_type.type_kind == TypeKind.INTERFACE

        # 构建参数声明列表
        # 注意: ABI 生成器对 [out] INTERFACE 参数会额外加一层指针
        # (IDL: IDasImage* → ABI: IDasImage**)，所以 director override
        # 必须匹配 ABI 签名而非 IDL 原文。
        param_decls = []
        for param in method.parameters:
            param_type = self._get_sol2_param_type(param.type_info)
            # ABI 生成器对 [out] INTERFACE 且 pointer_level==1 的参数
            # 会额外加一层指针 (IDL: T* → ABI: T**)。
            # pointer_level>=2 的情况 ABI 不再加 (IDL: T** → ABI: T**)。
            # 非 INTERFACE [out] 参数 ABI 不加指针。
            if (param.direction == ParamDirection.OUT
                    and param.type_info.type_kind == TypeKind.INTERFACE
                    and param.type_info.pointer_level == 1):
                param_type = f"{param_type}*"
            param_decls.append(f'{param_type} {param.name}')
        param_list = ', '.join(param_decls) if param_decls else ''

        # 构建父接口调用的实参列表（所有参数）
        all_args = ', '.join(p.name for p in method.parameters)

        # 构建 Lua 调用的实参列表（跳过 [out] 参数）
        lua_args_list = []
        for param in method.parameters:
            if param.direction != ParamDirection.OUT:
                lua_args_list.append(param.name)
        lua_args = ', '.join(lua_args_list)

        lines = []
        lines.append('')
        lines.append(f'    {ret_type} {method.name}({param_list}) override {{')

        # 路径 1: upcall 重入 或 Lua 方法不存在 → 返回安全默认值
        # 注意: 不调用 parent::method() 因为接口方法是纯虚函数 (= 0)，没有实现
        lines.append(
            f'        if (is_upcall_active()'
            f' || !has_lua_method("{method.name}")) {{'
        )
        if is_void:
            lines.append('            return;')
        elif is_das_result:
            lines.append('            return DAS_E_NOT_FOUND;')
        elif is_interface_ptr:
            lines.append('            return nullptr;')
        else:
            lines.append(f'            return {ret_type}();')
        lines.append('        }')
        lines.append('')

        # 路径 2: 调用 Lua 方法
        lines.append('        UpcallGuard guard(this);')
        if lua_args:
            lines.append(
                f'        auto lua_result = call_lua_method("{method.name}",'
                f' {lua_args});'
            )
        else:
            lines.append(
                f'        auto lua_result = call_lua_method("{method.name}");'
            )
        lines.append('        if (!lua_result.valid()) {')
        lines.append(
            '            // Lua call failed — return safe error value'
        )

        # 错误回退 — 不调用纯虚函数
        if is_void:
            lines.append('            return;')
        elif is_das_result:
            lines.append('            return DAS_E_FAIL;')
        elif is_interface_ptr:
            lines.append('            return nullptr;')
        else:
            lines.append(f'            return {ret_type}();')

        lines.append('        }')

        # 提取返回值（void 无需提取）
        if not is_void:
            lines.append(f'        return lua_result.get<{ret_type}>();')

        lines.append('    }')

        return '\n'.join(lines)

    def _get_out_param_local_type(self, type_info: TypeInfo) -> str:
        """获取 [out] 参数局部变量的 C++ 值类型（去除外层指针）。

        用于 sol2 注册 lambda 中声明局部变量：
          - INTERFACE → TypeName*（接口使用指针作为值类型）
          - 其他类型 → base_type + (pointer_level-1) 个 *（去除最外层指针）

        Args:
            type_info: 经 resolve_types() 标注后的类型信息。

        Returns:
            适合用于局部变量声明的 C++ 类型字符串。
        """
        if type_info.type_kind == TypeKind.INTERFACE:
            # [out] InterfaceType** → 局部变量类型是 InterfaceType*
            # pointer_level 为 out 参数的指针层级（通常为 2），减 1 得到值类型
            if type_info.pointer_level > 1:
                return f"{type_info.base_type}{'*' * (type_info.pointer_level - 1)}"
            return f"{type_info.base_type}*"
        # 非 INTERFACE 类型：去除最外层指针（pointer_level - 1）
        if type_info.pointer_level > 1:
            return f"{type_info.base_type}{'*' * (type_info.pointer_level - 1)}"
        return type_info.base_type

    def _generate_out_param_lambda(
        self, iface_name: str, method: MethodDef
    ) -> str:
        """为含有 [out] 参数的方法生成 sol2 lambda 包装。

        Lambda 将 [out] 参数转为返回值 tuple 的一部分，
        使 Lua 端可以通过多返回值获取 [out] 参数的输出。

        Args:
            iface_name: 接口名称。
            method: 方法定义。

        Returns:
            Lambda 表达式字符串（不含 sol::overload 外壳）。
        """
        out_params = [
            p for p in method.parameters if p.direction == ParamDirection.OUT
        ]
        in_params = [
            p for p in method.parameters if p.direction != ParamDirection.OUT
        ]

        # Lambda 参数: InterfaceName& self + 所有 IN/INOUT 参数
        lambda_params = [f'{iface_name}& self']
        for p in in_params:
            param_type = self._get_sol2_param_type(p.type_info)
            lambda_params.append(f'{param_type} {p.name}')

        # 返回类型: std::tuple<RetType, OutType1, ...>
        ret_type = self._get_sol2_return_type(method.return_type)
        out_types = [
            self._get_out_param_local_type(p.type_info) for p in out_params
        ]

        if ret_type == 'void':
            tuple_types = ', '.join(out_types)
            ret_type_str = f'std::tuple<{tuple_types}>'
        else:
            all_types = [ret_type] + out_types
            tuple_types = ', '.join(all_types)
            ret_type_str = f'std::tuple<{tuple_types}>'

        lines = []
        params_str = ', '.join(lambda_params)
        lines.append(f'[]({params_str}) -> {ret_type_str} {{')

        # 声明 [out] 参数的局部变量（零初始化）
        for p in out_params:
            local_type = self._get_out_param_local_type(p.type_info)
            lines.append(f'    {local_type} {p.name}{{}};')

        # 构建方法调用的实参列表（保持原始参数顺序）
        call_args = []
        for p in method.parameters:
            if p.direction == ParamDirection.OUT:
                call_args.append(f'&{p.name}')
            else:
                call_args.append(p.name)
        call_args_str = ', '.join(call_args)

        # 方法调用
        if ret_type == 'void':
            lines.append(f'    self.{method.name}({call_args_str});')
            out_values = ', '.join(p.name for p in out_params)
            lines.append(f'    return std::make_tuple({out_values});')
        else:
            lines.append(
                f'    {ret_type} hr = self.{method.name}({call_args_str});'
            )
            lines.append(
                '    if (hr < 0) {'
            )
            lines.append(
                '        // Method call failed — do not use uninitialized out values'
            )
            all_values = ['hr'] + [f'{p.name}' for p in out_params]
            values_str = ', '.join(all_values)
            lines.append(
                f'        return std::make_tuple({values_str});'
            )
            lines.append('    }')
            all_values = ['hr'] + [p.name for p in out_params]
            values_str = ', '.join(all_values)
            lines.append(f'    return std::make_tuple({values_str});')

        lines.append('}')

        return '\n'.join(lines)

    def _generate_binary_buffer_lambda(
        self, iface_name: str, method: MethodDef, interface: InterfaceDef
    ) -> str:
        """为 [binary_buffer] 标记的方法生成特殊的 sol2 lambda 包装。

        Lambda 调用 binary_buffer 方法获取数据指针，再调用同接口中的
        companion size 方法获取大小，将二进制数据拷贝到 std::string，
        返回 std::tuple<DasResult, std::string>。

        companion size 方法的查找规则：在同一接口中查找具有单个
        [out] uint64_t* 参数的方法。

        Args:
            iface_name: 接口名称。
            method: 被标记为 [binary_buffer] 的方法。
            interface: 所属接口定义（用于查找 companion size 方法）。

        Returns:
            Lambda 表达式字符串（不含 sol::overload 外壳）。
        """
        # 查找 companion size 方法（具有单个 [out] uint64_t* 参数）
        size_method_name = None
        for m in interface.methods:
            if m.name == method.name:
                continue
            out_ps = [
                p
                for p in m.parameters
                if p.direction == ParamDirection.OUT
            ]
            if len(out_ps) == 1 and out_ps[0].type_info.base_type == 'uint64_t':
                size_method_name = m.name
                break

        if size_method_name is None:
            # 回退到命名约定: GetData → GetSize
            size_method_name = method.name.replace('Data', 'Size')

        lines = []
        lines.append(
            f'[]({iface_name}& self)'
            f' -> std::tuple<DasResult, std::string> {{'
        )
        lines.append('    unsigned char* data = nullptr;')
        lines.append(f'    DasResult hr = self.{method.name}(&data);')
        lines.append('    if (hr < 0) {')
        lines.append(
            '        return std::make_tuple(hr, std::string());'
        )
        lines.append('    }')
        lines.append('    uint64_t size = 0;')
        lines.append(f'    hr = self.{size_method_name}(&size);')
        lines.append('    if (hr < 0) {')
        lines.append(
            '        return std::make_tuple(hr, std::string());'
        )
        lines.append('    }')
        lines.append(
            '    return std::make_tuple(DAS_S_OK,'
            ' std::string(reinterpret_cast<const char*>(data),'
            ' static_cast<size_t>(size)));'
        )
        lines.append('}')

        return '\n'.join(lines)

    def _generate_abstract_registration(self, interface: InterfaceDef) -> str:
        """生成抽象接口的 sol2 usertype 注册代码。

        抽象接口使用 sol::no_constructor 阻止 Lua 端直接实例化。
        方法绑定规则：
          - [binary_buffer] 标记 → 特殊 lambda：GetData+GetSize → std::string
          - 无 [out] 参数 → 直接绑定成员函数指针
          - 有 [out] 参数 → 使用 lambda 包装为 tuple 返回值

        Args:
            interface: 经 resolve_types() 标注后的接口定义。

        Returns:
            lua.new_usertype<Interface>(...) 调用的 C++ 代码字符串。
        """
        iface_name = interface.name

        entries = []
        entries.append('    sol::no_constructor')
        entries.append(
            f'    sol::meta_function::garbage_collect, []({iface_name}* p)'
            f' {{ p->Release(); }}'
        )

        for method in interface.methods:
            out_params = [
                p
                for p in method.parameters
                if p.direction == ParamDirection.OUT
            ]

            # [binary_buffer] 方法：生成特殊的 lambda，
            # 调用 GetData + GetSize 并将数据拷贝到 std::string
            if method.attributes.get('binary_buffer'):
                lambda_code = self._generate_binary_buffer_lambda(
                    iface_name, method, interface
                )
                overload_lines = []
                overload_lines.append(
                    f'    "{method.name}", sol::overload('
                )
                for line in lambda_code.split('\n'):
                    overload_lines.append(f'        {line}')
                overload_lines.append('    )')
                entries.append('\n'.join(overload_lines))
            elif not out_params:
                entries.append(
                    f'    "{method.name}", &{iface_name}::{method.name}'
                )
            else:
                lambda_code = self._generate_out_param_lambda(
                    iface_name, method
                )
                overload_lines = []
                overload_lines.append(
                    f'    "{method.name}", sol::overload('
                )
                for line in lambda_code.split('\n'):
                    overload_lines.append(f'        {line}')
                overload_lines.append('    )')
                entries.append('\n'.join(overload_lines))

        body = ',\n'.join(entries)
        return (
            f'lua.new_usertype<{iface_name}>("{iface_name}",\n'
            f'{body}\n'
            f');'
        )

    def _generate_director_registration(self, interface: InterfaceDef) -> str:
        """生成 Director 具体类的 sol2 usertype 注册代码。

        Director 类使用 sol::call_constructor 允许 Lua 端通过
        构造函数语法创建实例（传入 Lua table 作为方法覆盖表），
        并通过 sol::bases 声明与抽象接口的继承关系。

        Args:
            interface: 经 resolve_types() 标注后的接口定义。

        Returns:
            lua.new_usertype<DirectorClass>(...) 调用的 C++ 代码字符串。
        """
        iface_name = interface.name
        director_name = self._get_director_class_name(iface_name)

        return (
            f'lua.new_usertype<{director_name}>("{director_name}",\n'
            f'    sol::call_constructor,\n'
            f'    sol::initializers(\n'
            f'        []({director_name}& obj, sol::this_state ts,'
            f' sol::table env) {{\n'
            f'            new (&obj) {director_name}(ts, env);\n'
            f'        }}\n'
            f'    ),\n'
            f'    sol::base_classes,\n'
            f'    sol::bases<{iface_name}>(),\n'
            f'    sol::meta_function::garbage_collect,'
            f' []({director_name}* p) {{\n'
            f'        p->Release();\n'
            f'    }}\n'
            f');'
        )

    def _generate_registration_function(
        self, interfaces: List[InterfaceDef]
    ) -> str:
        """生成主注册函数 register_all_das_interfaces。

        该函数将所有抽象接口和 Director 具体类注册到 sol::state 中，
        使得 Lua 端可以使用这些类型。

        注册顺序：先注册所有抽象接口（包含方法绑定），
        再注册所有 Director 类（包含构造器和继承声明）。

        Args:
            interfaces: 经 resolve_types() 标注后的 InterfaceDef 列表。

        Returns:
            register_all_das_interfaces 函数的完整 C++ 定义字符串。
        """
        lines = []
        lines.append('void register_all_das_interfaces(sol::state_view lua) {')

        # 抽象接口注册
        if interfaces:
            lines.append('    // Abstract interfaces')
            for interface in interfaces:
                reg = self._generate_abstract_registration(interface)
                for line in reg.split('\n'):
                    lines.append(f'    {line}')
                lines.append('')

        # Director 具体类注册
        if interfaces:
            lines.append('    // Director concrete classes')
            for interface in interfaces:
                reg = self._generate_director_registration(interface)
                for line in reg.split('\n'):
                    lines.append(f'    {line}')
                lines.append('')

        lines.append('}')

        return '\n'.join(lines)

    def _generate_errorcode_binding(self, doc: IdlDocument) -> str:
        """生成错误码枚举的 sol2 注册代码。

        将 IDL 中定义的错误码块注册为 Lua 枚举，
        使 Lua 端可以通过名称访问错误码常量。

        生成格式：
            lua.new_enum<int32_t>("DasResult",
                "DAS_S_OK", 0,
                "DAS_E_NO_INTERFACE", -1073750001,
                ...
            );

        Args:
            doc: 经 resolve_types() 标注后的 IDL 文档。

        Returns:
            register_error_codes 函数的完整 C++ 定义字符串。
        """
        lines = []
        lines.append('void register_error_codes(sol::state_view lua) {')

        for error_code in doc.error_codes:
            # sol2 new_enum: name-value pairs (no template arg needed)
            entries = []
            for val in error_code.values:
                entries.append(f'"{val.name}", {val.value}')

            entries_str = ',\n        '.join(entries)
            lines.append(
                f'    lua.new_enum("{error_code.name}",\n'
                f'        {entries_str}\n'
                f'    );'
            )
            lines.append('')

        lines.append('}')
        return '\n'.join(lines)

    def _generate_module_binding(
        self,
        doc: IdlDocument,
        available_types: set = None,
    ) -> str:
        """生成模块函数的 sol2 注册代码。

        遍历所有模块及其函数，根据是否有 [out] 参数生成不同的绑定：
          - 无 [out] 参数 → 直接绑定函数指针
          - [swig_ret] 函数 → DasRet lambda（解包 DasRetXxx 为 tuple）
          - 有 [out] 参数的 C ABI 函数 → lambda 包装为 tuple 返回值

        Args:
            doc: 经 resolve_types() 标注后的 IDL 文档。
            available_types: 有完整 ABI 头文件的接口名集合。为 None 表示不过滤。

        Returns:
            register_module_functions 函数的完整 C++ 定义字符串。
        """
        lines = []
        lines.append('void register_module_functions(sol::state_view lua) {')

        for module in doc.modules:
            for func in module.functions:
                # 跳过引用了不完整接口类型的函数（无 ABI 头文件的接口）
                if available_types is not None:
                    if self._func_references_unavailable_type(
                        func, available_types
                    ):
                        continue
                out_params = [
                    p
                    for p in func.parameters
                    if p.direction == ParamDirection.OUT
                ]

                if not out_params:
                    # 无 [out] 参数 → 直接绑定函数指针
                    lines.append(
                        f'    lua.set_function("{func.name}",'
                        f' &{func.name});'
                    )
                elif self._is_das_ret_function(func):
                    # [swig_ret] 函数 → DasRet lambda
                    lambda_lines = self._generate_das_ret_lambda(func)
                    lines.append(
                        f'    lua.set_function("{func.name}",'
                    )
                    for ll in lambda_lines:
                        lines.append(f'    {ll}')
                    lines.append('    );')
                else:
                    # 有 [out] 参数的 C ABI 函数 → 原始 lambda
                    lambda_lines = (
                        self._generate_module_function_lambda(func)
                    )
                    lines.append(
                        f'    lua.set_function("{func.name}",'
                    )
                    for ll in lambda_lines:
                        lines.append(f'    {ll}')
                    lines.append('    );')

            lines.append('')

        lines.append('}')
        return '\n'.join(lines)

    # IDL 类型名到 C++ 实际类型的映射不一致列表。
    # 这些 IDL 类型名在生成的 C++ 代码中会导致签名不匹配的链接错误。
    _IDL_CPP_SIGNATURE_MISMATCH_TYPES = frozenset({
        'DasReadOnlyString',   # IDL: DasReadOnlyString → C++: IDasReadOnlyString*
        'DasString',           # IDL: DasString → C++: IDasString*
    })

    @staticmethod
    def _is_das_ret_function(func) -> bool:
        """Check if a module function uses DasRet return pattern.

        Only functions with [swig_ret] attribute should generate DasRet lambda.
        """
        return bool(func.attributes.get('swig_ret', False))

    def _generate_das_ret_lambda(self, func) -> List[str]:
        """为 [swig_ret] 模块函数生成 sol2 lambda 包装。

        DasRet 函数的签名已被 header_generator 转换：
        - [out] 参数被移除
        - 返回类型变为 DasRetXxx

        Lambda 直接调用函数，将 DasRetXxx 解包为 std::tuple<DasResult, OutType>。
        """
        # Lambda 参数：仅 IN/INOUT 参数（out 参数已被 header_generator 移除）
        in_params = [
            p for p in func.parameters if p.direction != ParamDirection.OUT
        ]
        lambda_params = []
        for p in in_params:
            param_type = self._get_sol2_param_type(p.type_info)
            lambda_params.append(f'{param_type} {p.name}')

        # 返回类型：std::tuple<DasResult, OutType>
        # DasRet 函数返回 DasResult，但实际返回类型是 DasRetXxx
        ret_type = self._get_sol2_return_type(func.return_type)
        out_params = [
            p for p in func.parameters if p.direction == ParamDirection.OUT
        ]
        out_types = [
            self._get_out_param_local_type(p.type_info) for p in out_params
        ]

        all_types = [ret_type] + out_types
        tuple_types = ', '.join(all_types)
        ret_type_str = f'std::tuple<{tuple_types}>'

        lines = []
        params_str = ', '.join(lambda_params)
        lines.append(f'[]({params_str}) -> {ret_type_str} {{')

        # 调用函数并解包 DasRet
        in_args = [p.name for p in in_params]
        call_args_str = ', '.join(in_args)
        lines.append(f'    auto ret = {func.name}({call_args_str});')

        # 解包 DasRet → (error_code, value)
        out_values = ['ret.GetErrorCode()'] + [
            f'ret.GetValue()' for _ in out_params
        ]
        values_str = ', '.join(out_values)
        lines.append(f'    return std::make_tuple({values_str});')

        lines.append('}')
        return lines

    @staticmethod
    def _func_references_unavailable_type(
        func, available_types: set
    ) -> bool:
        """检查函数是否引用了没有完整定义的接口类型或签名不匹配的类型。

        Args:
            func: 模块函数定义（ModuleFunctionDef）。
            available_types: 有完整 ABI 头文件的接口名集合。

        Returns:
            True 如果函数引用了不可用的接口类型。
        """
        def _check_type(type_info):
            # INTERFACE 类型直接检查
            if type_info.type_kind == TypeKind.INTERFACE:
                return type_info.base_type not in available_types
            # UNKNOWN 类型可能是带命名空间前缀的接口
            if type_info.type_kind == TypeKind.UNKNOWN:
                # 提取短名（去掉命名空间前缀）
                short_name = type_info.base_type.rsplit('::', 1)[-1]
                if short_name.startswith('IDas') and short_name not in available_types:
                    return True
            # 检查签名不匹配的类型（IDL 名与 C++ 名不一致）
            if type_info.base_type in LuaSwigGenerator._IDL_CPP_SIGNATURE_MISMATCH_TYPES:
                return True
            return False

        # Check return type
        if _check_type(func.return_type):
            return True
        # Check parameters
        for p in func.parameters:
            if _check_type(p.type_info):
                return True
        return False

    def _generate_module_function_lambda(
        self, func: ModuleFunctionDef
    ) -> List[str]:
        """为含有 [out] 参数的模块函数生成 sol2 lambda 包装。

        Lambda 将 [out] 参数转为返回值 tuple 的一部分，
        使 Lua 端可以通过多返回值获取 [out] 参数的输出。

        Args:
            func: 模块函数定义。

        Returns:
            Lambda 表达式的代码行列表。
        """
        out_params = [
            p for p in func.parameters if p.direction == ParamDirection.OUT
        ]
        in_params = [
            p for p in func.parameters if p.direction != ParamDirection.OUT
        ]

        # Lambda 参数列表：仅 IN/INOUT 参数
        lambda_params = []
        for p in in_params:
            param_type = self._get_sol2_param_type(p.type_info)
            lambda_params.append(f'{param_type} {p.name}')

        # 返回类型
        ret_type = self._get_sol2_return_type(func.return_type)
        out_types = [
            self._get_out_param_local_type(p.type_info) for p in out_params
        ]

        if ret_type == 'void':
            # void 返回 + [out]：tuple 仅包含 [out] 类型
            tuple_types = ', '.join(out_types)
            ret_type_str = f'std::tuple<{tuple_types}>'
        else:
            all_types = [ret_type] + out_types
            tuple_types = ', '.join(all_types)
            ret_type_str = f'std::tuple<{tuple_types}>'

        lines = []
        params_str = ', '.join(lambda_params)
        lines.append(f'[]({params_str}) -> {ret_type_str} {{')

        # 声明 [out] 参数的局部变量（零初始化）
        for p in out_params:
            local_type = self._get_out_param_local_type(p.type_info)
            lines.append(f'    {local_type} {p.name}{{}};')

        # 构建函数调用的实参列表
        call_args = []
        for p in func.parameters:
            if p.direction == ParamDirection.OUT:
                call_args.append(f'&{p.name}')
            else:
                call_args.append(p.name)
        call_args_str = ', '.join(call_args)

        # 函数调用
        if ret_type == 'void':
            lines.append(f'    {func.name}({call_args_str});')
            out_values = ', '.join(p.name for p in out_params)
            lines.append(f'    return std::make_tuple({out_values});')
        else:
            lines.append(
                f'    {ret_type} hr = {func.name}({call_args_str});'
            )
            lines.append(
                '    if (hr < 0) {'
            )
            lines.append(
                '        // Function call failed — do not use uninitialized out values'
            )
            all_values = ['hr'] + [f'{p.name}' for p in out_params]
            values_str = ', '.join(all_values)
            lines.append(
                f'        return std::make_tuple({values_str});'
            )
            lines.append('    }')
            all_values = ['hr'] + [p.name for p in out_params]
            values_str = ', '.join(all_values)
            lines.append(f'    return std::make_tuple({values_str});')

        lines.append('}')

        return lines

    # ── EmmyLua 类型映射 ──────────────────────────────────────────────

    _INTEGER_TYPES = frozenset({
        'int32_t', 'int32', 'int', 'int64', 'uint32', 'uint64',
        'size_t', 'int8', 'uint8', 'int16', 'uint16',
        'int64_t', 'uint32_t', 'uint64_t', 'size_t',
    })
    _NUMBER_TYPES = frozenset({'float', 'double'})
    _BOOLEAN_TYPES = frozenset({'bool', 'DasBool'})
    _STRING_TYPES = frozenset({
        'DasString', 'DasReadOnlyString', 'IDasReadOnlyString', 'char',
    })

    def _emmy_lua_type(self, type_info: TypeInfo) -> str:
        """将 IDL TypeInfo 映射为 EmmyLua 注解类型字符串。

        映射规则:
          - 整数族 (int32_t, uint32, ...) → "integer"
          - 浮点族 (float, double) → "number"
          - 布尔族 (bool, DasBool) → "boolean"
          - 字符串族 (DasString, char*, ...) → "string"
          - DasResult → "DasResult"
          - DasGuid → "DasGuid"
          - INTERFACE* → 接口名称（去指针）
          - unsigned char* / uint8_t* → "string"（二进制缓冲区）
          - void → "nil"
          - 其他 → base_type

        Args:
            type_info: 经 resolve_types() 标注后的类型信息。

        Returns:
            EmmyLua 类型字符串。
        """
        base = type_info.base_type

        if base == 'void':
            return 'nil'
        if base == 'DasResult':
            return 'DasResult'
        if base == 'DasGuid':
            return 'DasGuid'
        if base in self._INTEGER_TYPES:
            return 'integer'
        if base in self._NUMBER_TYPES:
            return 'number'
        if base in self._BOOLEAN_TYPES:
            return 'boolean'
        if base in self._STRING_TYPES:
            return 'string'
        # unsigned char* / uint8_t* → binary buffer → string
        if base in ('unsigned char', 'uint8_t') and type_info.is_pointer:
            return 'string'
        # char* → string
        if base == 'char' and type_info.is_pointer:
            return 'string'
        # Interface pointer → interface name
        if type_info.type_kind == TypeKind.INTERFACE:
            return base
        # Fallback: use base_type as-is
        return base

    def _generate_lua_stub(
        self, interfaces: List[InterfaceDef], doc: IdlDocument
    ) -> str:
        """生成纯 EmmyLua 注解的 .lua 桩文件。

        生成的 .lua 文件仅供 LuaLS 类型检查使用，不含任何运行时代码。
        格式为 100% EmmyLua 注解（---@class / ---@field / ---@param /
        ---@return / ---@type），不包含空 table、空函数体或 SWIG 代码。

        Args:
            interfaces: 经 resolve_types() 标注后的 InterfaceDef 列表。
            doc: 经 resolve_types() 标注后的 IDL 文档。

        Returns:
            完整的 .lua 桩文件内容字符串。
        """
        lines: List[str] = []

        # ── 文件头 ────────────────────────────────────────────────────
        lines.append(
            '--- Das Core Lua type stubs (auto-generated — DO NOT MODIFY)'
        )
        lines.append(
            '--- Runtime provided by DasCoreLuaExport.dll'
        )
        lines.append('')

        # ── DasGuid（不透明类型）──────────────────────────────────────
        lines.append('---@class DasGuid')
        lines.append('')

        # ── DasResult 别名 + 常量 ─────────────────────────────────────
        lines.append('---@alias DasResult integer')
        for error_code in doc.error_codes:
            for val in error_code.values:
                lines.append(
                    f'---@type DasResult {val.name} = {val.value}'
                )
        lines.append('')

        # ── 接口类注解 ────────────────────────────────────────────────
        for interface in interfaces:
            iface_name = interface.name
            base = interface.base_interface

            # ---@class [Name] [: Base]
            if base and base != 'IDasBase':
                lines.append(f'---@class {iface_name} : {base}')
            else:
                lines.append(f'---@class {iface_name}')

            # 为每个方法生成 ---@field fun(self, params): returns
            for method in interface.methods:
                param_parts = ['self']
                for param in method.parameters:
                    if param.direction != ParamDirection.OUT:
                        emmy_type = self._emmy_lua_type(param.type_info)
                        param_parts.append(f'{param.name}: {emmy_type}')

                # 返回值: DasResult + out 参数
                return_parts = []
                ret_type = method.return_type
                if ret_type.base_type != 'void':
                    return_parts.append(self._emmy_lua_type(ret_type))
                for param in method.parameters:
                    if param.direction in (ParamDirection.OUT, ParamDirection.INOUT):
                        return_parts.append(
                            self._emmy_lua_type(param.type_info)
                        )

                params_str = ', '.join(param_parts)
                if return_parts:
                    returns_str = ', '.join(return_parts)
                    lines.append(
                        f'---@field {method.name} fun({params_str}):'
                        f' {returns_str}'
                    )
                else:
                    lines.append(
                        f'---@field {method.name} fun({params_str})'
                    )

            # 为每个属性生成 ---@field
            for prop in interface.properties:
                emmy_type = self._emmy_lua_type(prop.type_info)
                lines.append(f'---@field {prop.name} {emmy_type}')

            lines.append('')

        # ── Director 类注解 ──────────────────────────────────────────
        for interface in interfaces:
            iface_name = interface.name
            director_name = self._get_director_class_name(iface_name)
            lines.append(
                f'---@class {director_name} : {iface_name}'
            )
            lines.append(
                f'---@field new fun(self, env: table): {director_name}'
            )
            lines.append('')

        # ── 模块函数注解 ─────────────────────────────────────────────
        for module in doc.modules:
            for func in module.functions:
                # ---@param 注解
                for param in func.parameters:
                    if param.direction != ParamDirection.OUT:
                        emmy_type = self._emmy_lua_type(param.type_info)
                        lines.append(
                            f'---@param {param.name} {emmy_type}'
                        )

                # ---@return 注解
                return_parts = []
                if func.return_type.base_type != 'void':
                    return_parts.append(
                        self._emmy_lua_type(func.return_type)
                    )
                for param in func.parameters:
                    if param.direction in (ParamDirection.OUT, ParamDirection.INOUT):
                        return_parts.append(
                            self._emmy_lua_type(param.type_info)
                        )
                if return_parts:
                    lines.append(
                        f'---@return {", ".join(return_parts)}'
                    )

                # 函数声明（最小化：仅签名 + end）
                in_params = [
                    p for p in func.parameters
                    if p.direction != ParamDirection.OUT
                ]
                param_names = ', '.join(p.name for p in in_params)
                lines.append(f'function {func.name}({param_names}) end')
                lines.append('')

        return '\n'.join(lines)

    def _generate_luaopen_function(
        self, doc: IdlDocument, interfaces: List[InterfaceDef]
    ) -> str:
        """生成 DLL 入口函数 luaopen_<module_name>。

        该函数是 Lua require 系统的入口点，
        负责依次注册所有类型、枚举和模块函数。

        Args:
            doc: 经 resolve_types() 标注后的 IDL 文档。
            interfaces: 经 resolve_types() 标注后的 InterfaceDef 列表。

        Returns:
            luaopen_<module_name> 函数的完整 C++ 定义字符串。

        Raises:
            ValueError: 当 IDL 文档中未定义 module 声明时。
        """
        # 确定模块名（必须从 IDL module 声明获取，不使用硬编码默认值）
        if not doc.modules or not doc.modules[0].module_name:
            raise ValueError(
                "luaopen 函数需要模块名，但 IDL 文档中未定义 module 声明。"
                "请在 IDL 文件中添加 module 块，例如: module MyModuleName { }"
            )
        module_name = doc.modules[0].module_name

        lines = []
        lines.append(
            f'extern "C" int luaopen_{module_name}(lua_State* L) {{'
        )
        lines.append('    sol::state_view lua(L);')
        lines.append('')

        # 注册接口类型（仅在有接口时调用）
        if interfaces:
            lines.append('    // Register all types and Director classes')
            lines.append('    register_all_das_interfaces(lua);')
            lines.append('')

        # 注册错误码枚举
        if doc.error_codes:
            lines.append('    // Register error code enums')
            lines.append('    register_error_codes(lua);')
            lines.append('')

        # 注册模块函数
        has_functions = any(
            len(module.functions) > 0 for module in doc.modules
        )
        if has_functions:
            lines.append('    // Register module functions')
            lines.append('    register_module_functions(lua);')
            lines.append('')

        lines.append('    return 1;')
        lines.append('}')
        return '\n'.join(lines)
