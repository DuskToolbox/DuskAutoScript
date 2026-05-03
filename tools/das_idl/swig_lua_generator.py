"""
Lua SWIG 生成器

生成 Lua 特定的 SWIG .i 文件代码
"""

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
        # DasGuid 特殊处理：作为内置原始类型，按 const 引用传递
        if type_info.type_kind == TypeKind.BASIC and type_info.base_type == 'DasGuid':
            return "const DasGuid&"

        # 根据类型分类确定基础类型名
        if type_info.type_kind == TypeKind.INTERFACE:
            result = f"{type_info.base_type}*"
        else:
            # BASIC, ENUM, STRUCT, UNKNOWN → 直接使用 base_type
            result = type_info.base_type

        # 根据 const / reference / pointer 标志追加修饰
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

    def _generate_director_class(self, interface: InterfaceDef) -> str:
        """生成 ILuaDas{Name} Director 包装类的 C++ 定义。

        每个 Director 类同时继承 C++ 接口和 LuaDirector，
        提供 upcall 分发并带有回退到父接口默认实现的机制。

        命名规则:
          - IDasLogger → ILuaDasLogger（以 IDas 开头）
          - ILogger   → ILuaLogger（其他情况）

        Args:
            interface: 经 resolve_types() 标注后的接口定义。

        Returns:
            完整的 Director 类 C++ 定义字符串。
        """
        iface_name = interface.name
        class_name = self._get_director_class_name(iface_name)

        parent = iface_name

        lines = []
        lines.append(f'class {class_name} : public {parent}, public LuaDirector {{')
        lines.append('public:')
        lines.append(f'    explicit {class_name}(lua_State* L, sol::table env)')
        lines.append('        : LuaDirector()')
        lines.append('    {')
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
            '        uint32_t count = ref_count_.fetch_sub(1,'
            ' std::memory_order_acq_rel) - 1;'
        )
        lines.append('        if (count == 0) {')
        lines.append('            delete this;')
        lines.append('        }')
        lines.append('        return count;')
        lines.append('    }')
        lines.append('')
        lines.append(
            '    DasResult QueryInterface(const DasGuid& iid,'
            ' void** pp_object) override {'
        )
        lines.append(
            '        if (!pp_object) return DAS_E_INVALID_POINTER;'
        )
        lines.append(
            f'        if (iid == IDasBase::GetIID() || iid'
            f' == {parent}::GetIID()) {{'
        )
        lines.append(
            f'            *pp_object ='
            f' static_cast<{parent}*>(this);'
        )
        lines.append('            AddRef();')
        lines.append('            return DAS_S_OK;')
        lines.append('        }')
        lines.append('        *pp_object = nullptr;')
        lines.append('        return DAS_E_NO_INTERFACE;')
        lines.append('    }')

        # 为每个方法生成 override
        for method in interface.methods:
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
        param_decls = []
        for param in method.parameters:
            param_type = self._get_sol2_param_type(param.type_info)
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

        # 路径 1: upcall 重入 或 Lua 方法不存在 → 回退到父接口
        lines.append(
            f'        if (is_upcall_active()'
            f' || !has_lua_method("{method.name}")) {{'
        )
        if is_void:
            if all_args:
                lines.append(f'            {parent}::{method.name}({all_args});')
            else:
                lines.append(f'            {parent}::{method.name}();')
            lines.append('            return;')
        else:
            if all_args:
                lines.append(
                    f'            return {parent}::{method.name}({all_args});'
                )
            else:
                lines.append(
                    f'            return {parent}::{method.name}();'
                )
        lines.append('        }')
        lines.append('')

        # 路径 2: 调用 Lua 方法
        lines.append('        UpcallGuard guard(this);')
        if lua_args:
            lines.append(
                f'        auto result = call_lua_method("{method.name}",'
                f' {lua_args});'
            )
        else:
            lines.append(
                f'        auto result = call_lua_method("{method.name}");'
            )
        lines.append('        if (!result.valid()) {')
        lines.append(
            '            // Lua call failed — return safe error value'
        )

        # 错误回退
        if is_void:
            if all_args:
                lines.append(
                    f'            {parent}::{method.name}({all_args});'
                )
            else:
                lines.append(f'            {parent}::{method.name}();')
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
            lines.append(f'        return result.get<{ret_type}>();')

        lines.append('    }')

        return '\n'.join(lines)

    def _get_out_param_local_type(self, type_info: TypeInfo) -> str:
        """获取 [out] 参数局部变量的 C++ 值类型（去除外层指针）。

        用于 sol2 注册 lambda 中声明局部变量：
          - INTERFACE → TypeName*（接口使用指针作为值类型）
          - 其他类型 → base_type（去除指针修饰的原始类型）

        Args:
            type_info: 经 resolve_types() 标注后的类型信息。

        Returns:
            适合用于局部变量声明的 C++ 类型字符串。
        """
        if type_info.type_kind == TypeKind.INTERFACE:
            return f"{type_info.base_type}*"
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

    def _generate_abstract_registration(self, interface: InterfaceDef) -> str:
        """生成抽象接口的 sol2 usertype 注册代码。

        抽象接口使用 sol::no_constructor 阻止 Lua 端直接实例化。
        方法绑定规则：
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

        for method in interface.methods:
            out_params = [
                p
                for p in method.parameters
                if p.direction == ParamDirection.OUT
            ]

            if not out_params:
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
            f'        [](lua_State* L, sol::table env) {{\n'
            f'            return new {director_name}(L, env);\n'
            f'        }}\n'
            f'    ),\n'
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
        lines.append('void register_all_das_interfaces(sol::state& lua) {')

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
        lines.append('void register_error_codes(sol::state& lua) {')

        for error_code in doc.error_codes:
            # 使用 int32_t 作为底层类型
            entries = []
            for val in error_code.values:
                entries.append(f'"{val.name}", {val.value}')

            entries_str = ',\n        '.join(entries)
            lines.append(
                f'    lua.new_enum<int32_t>("{error_code.name}",\n'
                f'        {entries_str}\n'
                f'    );'
            )
            lines.append('')

        lines.append('}')
        return '\n'.join(lines)

    def _generate_module_binding(self, doc: IdlDocument) -> str:
        """生成模块函数的 sol2 注册代码。

        遍历所有模块及其函数，根据是否有 [out] 参数生成不同的绑定：
          - 无 [out] 参数 → 直接绑定函数指针
          - 有 [out] 参数 → 使用 lambda 包装为 tuple 返回值

        生成格式（无 [out]）：
            lua.set_function("DasLogError", &DasLogError);

        生成格式（有 [out]）：
            lua.set_function("FuncName", [](params...) -> std::tuple<...> {
                OutType out_param{};
                DasResult hr = FuncName(params..., &out_param);
                return std::make_tuple(hr, out_param);
            });

        Args:
            doc: 经 resolve_types() 标注后的 IDL 文档。

        Returns:
            register_module_functions 函数的完整 C++ 定义字符串。
        """
        lines = []
        lines.append('void register_module_functions(sol::state& lua) {')

        for module in doc.modules:
            for func in module.functions:
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
                else:
                    # 有 [out] 参数 → lambda 包装
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
        """生成 DLL 入口函数 luaopen_das_core。

        该函数是 Lua require 系统的入口点，
        负责依次注册所有类型、枚举和模块函数。

        Args:
            doc: 经 resolve_types() 标注后的 IDL 文档。
            interfaces: 经 resolve_types() 标注后的 InterfaceDef 列表。

        Returns:
            luaopen_das_core 函数的完整 C++ 定义字符串。
        """
        # 确定模块名
        module_name = "das_core"
        if doc.modules and doc.modules[0].module_name:
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
