#!/usr/bin/env python3
"""Generate Node/NAPI support files from DAS IDL definitions."""

from __future__ import annotations

from dataclasses import dataclass, replace
from typing import Iterable, Sequence

from das_idl_parser import (
    IdlDocument,
    InterfaceDef,
    MethodDef,
    ModuleFunctionDef,
    ParameterDef,
    ParamDirection,
    StructDef,
    TypeInfo,
    TypeKind,
)
from shared_utils import type_simple_name


_SIGNED_64_TYPES = frozenset({"int64", "int64_t"})
_UNSIGNED_64_TYPES = frozenset({"uint64", "uint64_t", "size_t"})
_SIGNED_32_TYPES = frozenset(
    {
        "int",
        "int8",
        "int16",
        "int32",
        "int8_t",
        "int16_t",
        "int32_t",
        "signed char",
        "signed short",
        "signed int",
    }
)
_UNSIGNED_32_TYPES = frozenset(
    {
        "uint8",
        "uint16",
        "uint32",
        "uint8_t",
        "uint16_t",
        "uint32_t",
        "unsigned char",
        "unsigned short",
        "unsigned int",
    }
)
_FLOAT_TYPES = frozenset({"float", "double"})
_BOOL_TYPES = frozenset({"bool", "DasBool"})
_STRING_TYPES = frozenset({"char", "DasString", "DasReadOnlyString"})
_BINARY_BUFFER_NAMES = frozenset(
    {
        "IDasBinaryBuffer",
        "IDasMemory",
        "IDasImage",
        "IDasTensor",
    }
)


@dataclass(frozen=True)
class NapiArtifacts:
    cpp: str
    dts: str
    js: str


@dataclass(frozen=True)
class FunctionSupport:
    name: str
    supported: bool
    reason: str = ""


@dataclass(frozen=True)
class _MappedType:
    cpp_type: str
    ts_type: str
    js_check: str
    category: str


def _cpp_type(type_info: TypeInfo) -> str:
    base = type_simple_name(type_info)
    if type_info.is_qualified and type_info.resolved_qualified_name:
        base = type_info.resolved_qualified_name
    result = base
    if type_info.is_const:
        result = f"const {result}"
    if type_info.is_pointer:
        result = f"{result}{'*' * type_info.pointer_level}"
    if type_info.is_reference:
        result = f"{result}&"
    return result


def _is_string_pointer(type_info: TypeInfo) -> bool:
    return (
        type_simple_name(type_info) == "char"
        and type_info.is_pointer
        and type_info.pointer_level == 1
    )


def _is_readonly_string_interface_pointer(type_info: TypeInfo) -> bool:
    return (
        type_simple_name(type_info) == "IDasReadOnlyString"
        and type_info.type_kind == TypeKind.INTERFACE
        and type_info.is_pointer
        and type_info.pointer_level == 1
    )


def _is_binary_buffer_like(type_info: TypeInfo) -> bool:
    base = type_simple_name(type_info)
    if base in _BINARY_BUFFER_NAMES and type_info.is_pointer:
        return True
    return base in {"uint8_t", "uint8", "unsigned char"} and type_info.is_pointer


def _map_type(type_info: TypeInfo, *, for_return: bool = False) -> _MappedType | None:
    base = type_simple_name(type_info)

    if base == "void" and for_return and not type_info.is_pointer:
        return _MappedType("void", "void", "undefined", "void")
    if base == "DasResult" and not type_info.is_pointer:
        return _MappedType("DasResult", "DasResult", "number", "das_result")
    if base == "DasGuid" and not type_info.is_pointer:
        return _MappedType(_cpp_type(type_info), "DasGuid", "string", "das_guid")
    if base in _SIGNED_64_TYPES and not type_info.is_pointer:
        return _MappedType(_cpp_type(type_info), "bigint", "bigint", "int64")
    if base in _UNSIGNED_64_TYPES and not type_info.is_pointer:
        return _MappedType(_cpp_type(type_info), "bigint", "bigint", "uint64")
    if base in _SIGNED_32_TYPES and not type_info.is_pointer:
        return _MappedType(_cpp_type(type_info), "number", "number", "int32")
    if base in _UNSIGNED_32_TYPES and not type_info.is_pointer:
        return _MappedType(_cpp_type(type_info), "number", "number", "uint32")
    if base in _FLOAT_TYPES and not type_info.is_pointer:
        return _MappedType(_cpp_type(type_info), "number", "number", "number")
    if base in _BOOL_TYPES and not type_info.is_pointer:
        return _MappedType(_cpp_type(type_info), "boolean", "boolean", "boolean")
    if _is_string_pointer(type_info):
        return _MappedType(_cpp_type(type_info), "string", "string", "utf8_string")
    if _is_readonly_string_interface_pointer(type_info):
        return _MappedType(_cpp_type(type_info), "string", "string", "readonly_string")
    if type_info.type_kind == TypeKind.ENUM and not type_info.is_pointer:
        return _MappedType(_cpp_type(type_info), "number", "number", "enum")

    return None


def _unsupported_reason_for_type(type_info: TypeInfo, *, for_return: bool = False) -> str:
    if _is_binary_buffer_like(type_info):
        return "binary buffer inputs are deferred to Phase 74"
    if type_info.type_kind == TypeKind.STRUCT:
        return "struct values are deferred to Phase 74"
    if type_info.type_kind == TypeKind.INTERFACE:
        if for_return:
            return "interface returns are deferred to Phase 74"
        return "interface pointer inputs are deferred to Phase 74"
    if type_info.type_kind == TypeKind.UNKNOWN and type_simple_name(type_info).startswith("I"):
        return "interface pointer inputs are deferred to Phase 74"
    if type_info.is_pointer:
        return "pointer inputs are deferred to Phase 74"
    return f"unsupported type {type_simple_name(type_info)}"


def classify_module_function(func: ModuleFunctionDef) -> FunctionSupport:
    lowered = func.name.lower()
    if "bootstrap" in lowered or "nodehost" in lowered or "hostbootstrap" in lowered:
        return FunctionSupport(
            func.name,
            False,
            "Node host/bootstrap is deferred to Phase 75",
        )

    for param in func.parameters:
        if param.direction != ParamDirection.IN:
            return FunctionSupport(
                func.name,
                False,
                "out/inout parameters are deferred to Phase 74",
            )

    if _map_type(func.return_type, for_return=True) is None:
        return FunctionSupport(
            func.name,
            False,
            _unsupported_reason_for_type(func.return_type, for_return=True),
        )

    for param in func.parameters:
        if _map_type(param.type_info) is None:
            return FunctionSupport(
                func.name,
                False,
                _unsupported_reason_for_type(param.type_info),
            )

    return FunctionSupport(func.name, True)


def _iter_module_functions(doc: IdlDocument) -> Iterable[ModuleFunctionDef]:
    for module in doc.modules:
        yield from module.functions


def _escape_cpp_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def _escape_js_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace("'", "\\'")


def _sanitize_identifier(name: str) -> str:
    return "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in name)


def _generated_header_name(abi_header_name: str) -> str:
    if abi_header_name.endswith(".h"):
        return f"{abi_header_name[:-2]}.generated.h"
    return f"{abi_header_name}.generated.h"


def _cpp_wrapper_name(interface_name: str) -> str:
    return f"{_sanitize_identifier(interface_name)}Wrapper"


def _wrapped_interface_names(doc: IdlDocument) -> list[str]:
    names: list[str] = []
    if not any(interface.name == "IDasBase" for interface in doc.interfaces):
        names.append("IDasBase")
    names.extend(interface.name for interface in doc.interfaces)
    return names


def public_js_name(idl_name: str) -> str:
    """Convert public IDL PascalCase/camel names to lower camel JavaScript names."""
    if not idl_name:
        return idl_name
    if "_" not in idl_name:
        return f"{idl_name[0].lower()}{idl_name[1:]}"
    parts = [part for part in idl_name.split("_") if part]
    if not parts:
        return idl_name
    first = f"{parts[0][0].lower()}{parts[0][1:]}" if parts[0] else ""
    return first + "".join(f"{part[0].upper()}{part[1:]}" for part in parts[1:])


def clean_out_field_name(param_name: str) -> str:
    """Strip common DAS ABI out prefixes and lower-camel the public field name."""
    for prefix in ("pp_out_", "p_out_", "out_"):
        if param_name.startswith(prefix):
            return public_js_name(param_name[len(prefix):])
    return public_js_name(param_name)


def _value_type_for_out_param(param: ParameterDef) -> TypeInfo:
    type_info = param.type_info
    pointer_level = max(type_info.pointer_level - 1, 0)
    return replace(
        type_info,
        is_pointer=pointer_level > 0,
        pointer_level=pointer_level,
    )


def _is_binary_buffer_interface(type_info: TypeInfo) -> bool:
    return (
        type_simple_name(type_info) == "IDasBinaryBuffer"
        and type_info.type_kind == TypeKind.INTERFACE
    )


def _ts_type_for_struct_field(type_name: str) -> str:
    if type_name in _SIGNED_64_TYPES or type_name in _UNSIGNED_64_TYPES:
        return "bigint"
    if type_name in _SIGNED_32_TYPES or type_name in _UNSIGNED_32_TYPES:
        return "number"
    if type_name in _FLOAT_TYPES:
        return "number"
    if type_name in _BOOL_TYPES:
        return "boolean"
    return "unknown"


def _ts_type_for_out_param(param: ParameterDef) -> str:
    value_type = _value_type_for_out_param(param)
    if _is_binary_buffer_interface(value_type):
        return "Buffer"
    if value_type.type_kind == TypeKind.INTERFACE:
        return type_simple_name(value_type)
    if value_type.type_kind == TypeKind.STRUCT:
        return type_simple_name(value_type)

    mapped = _map_type(value_type)
    if mapped is not None:
        return mapped.ts_type
    return "unknown"


def _method_in_params(method: MethodDef) -> list[ParameterDef]:
    return [param for param in method.parameters if param.direction == ParamDirection.IN]


def _method_out_params(method: MethodDef) -> list[ParameterDef]:
    return [param for param in method.parameters if param.direction == ParamDirection.OUT]


def _method_ts_params(method: MethodDef) -> str:
    params = []
    for param in _method_in_params(method):
        mapped = _map_type(param.type_info)
        params.append(f"{public_js_name(param.name)}: {mapped.ts_type if mapped else 'unknown'}")
    return ", ".join(params)


def _method_ts_return(method: MethodDef) -> str:
    out_params = _method_out_params(method)
    if len(out_params) == 1:
        return _ts_type_for_out_param(out_params[0])
    if len(out_params) > 1:
        fields = [
            f"{clean_out_field_name(param.name)}: {_ts_type_for_out_param(param)};"
            for param in out_params
        ]
        return "{ " + " ".join(fields) + " }"

    mapped_return = _map_type(method.return_type, for_return=True)
    if mapped_return is not None and mapped_return.category != "void":
        return mapped_return.ts_type
    return "void"


class NapiGenerator:
    def __init__(
        self,
        *,
        package_name: str,
        addon_name: str,
        idl_header_names: Sequence[str] | None = None,
    ):
        if not package_name:
            raise ValueError("package_name is required")
        if not addon_name:
            raise ValueError("addon_name is required")
        self.package_name = package_name
        self.addon_name = addon_name
        self.idl_header_names = tuple(idl_header_names or ())

    def generate(self, doc: IdlDocument) -> NapiArtifacts:
        support = {
            func.name: classify_module_function(func)
            for func in _iter_module_functions(doc)
        }
        return NapiArtifacts(
            cpp=self._generate_cpp(doc, support),
            dts=self._generate_dts(doc, support),
            js=self._generate_js(doc, support),
        )

    def _generate_cpp(
        self,
        doc: IdlDocument,
        support: dict[str, FunctionSupport],
    ) -> str:
        lines: list[str] = [
            "// DAS Node/NAPI bindings (auto-generated - DO NOT MODIFY)",
            '#include <napi.h>',
            "#include <memory>",
            "#include <string>",
            "#include <utility>",
            "",
            '#include "das/IDasBase.h"',
            '#include "das/DasPtr.hpp"',
            '#include "das/DasString.hpp"',
            '#include "das/DasApi.h"',
        ]
        for header in self.idl_header_names:
            lines.append(f'#include "das/_autogen/idl/abi/{header}"')
        for header in self.idl_header_names:
            lines.append(
                f'#include "das/_autogen/idl/header/{_generated_header_name(header)}"'
            )
        lines.extend(
            [
                "",
                "namespace {",
                "",
                "Napi::Value MakeUnsupported(",
                "    const Napi::CallbackInfo& info,",
                "    const char* name,",
                "    const char* reason) {",
                "    Napi::Env env = info.Env();",
                "    throw Napi::Error::New(",
                "        env,",
                "        std::string{name} + \" is unsupported: \" + reason);",
                "}",
                "",
                "Napi::Object MakeDasException(",
                "    Napi::Env env,",
                "    DasResult result,",
                "    const std::string& message) {",
                "    Napi::Error error = Napi::Error::New(env, message);",
                "    Napi::Object exception = error.Value();",
                "    exception.Set(\"name\", \"DasException\");",
                "    exception.Set(\"result\", Napi::Number::New(env, static_cast<double>(result)));",
                "    exception.Set(\"code\", Napi::Number::New(env, static_cast<double>(result)));",
                "    return exception;",
                "}",
                "",
                "void ThrowDasException(",
                "    Napi::Env env,",
                "    DasResult result,",
                "    const std::string& message) {",
                "    Napi::Object exception = MakeDasException(env, result, message);",
                "    Napi::Error error(env, exception);",
                "    error.ThrowAsJavaScriptException();",
                "}",
                "",
                "DasGuid ReadDasGuid(Napi::Env env, const Napi::Value& input) {",
                "    if (!input.IsString()) {",
                "        throw Napi::TypeError::New(env, \"DasGuid must be a string\");",
                "    }",
                "    const std::string value = input.As<Napi::String>().Utf8Value();",
                "    DasGuid guid{};",
                "    const DasResult result = DasMakeDasGuid(value.c_str(), &guid);",
                "    if (result < 0) {",
                "        throw Napi::Error::New(env, \"invalid DasGuid string\");",
                "    }",
                "    return guid;",
                "}",
                "",
                "Napi::Value WriteDasGuid(Napi::Env env, const DasGuid& guid) {",
                "    IDasReadOnlyString* guid_string = nullptr;",
                "    const DasResult result = DasGuidToString(&guid, &guid_string);",
                "    if (result < 0 || guid_string == nullptr) {",
                "        throw Napi::Error::New(env, \"failed to format DasGuid\");",
                "    }",
                "    const char* utf8 = nullptr;",
                "    const DasResult utf8_result = guid_string->GetUtf8(&utf8);",
                "    if (utf8_result < 0 || utf8 == nullptr) {",
                "        guid_string->Release();",
                "        throw Napi::Error::New(env, \"failed to read DasGuid string\");",
                "    }",
                "    Napi::Value value = Napi::String::New(env, utf8);",
                "    guid_string->Release();",
                "    return value;",
                "}",
                "",
                "Napi::Value Guid(const Napi::CallbackInfo& info) {",
                "    Napi::Env env = info.Env();",
                "    if (info.Length() != 1 || !info[0].IsString()) {",
                "        throw Napi::TypeError::New(env, \"guid(value) expects a string\");",
                "    }",
                "    const DasGuid parsed = ReadDasGuid(env, info[0]);",
                "    return WriteDasGuid(env, parsed);",
                "}",
                "",
            ]
        )

        lines.extend(self._generate_cpp_wrapper_base())
        lines.append("")

        for interface_name in _wrapped_interface_names(doc):
            lines.extend(self._generate_cpp_wrapper_class(interface_name))
            lines.append("")

        lines.extend(self._generate_cpp_base_extractor(doc))
        lines.append("")

        if not any(interface.name == "IDasBase" for interface in doc.interfaces):
            lines.extend(self._generate_cpp_dispose_function("IDasBase"))
            lines.append("")

        for interface in doc.interfaces:
            lines.extend(self._generate_cpp_interface_helpers(interface, doc))
            lines.append("")

        for func in _iter_module_functions(doc):
            if support[func.name].supported:
                lines.extend(self._generate_supported_cpp_function(func))
            else:
                lines.extend(self._generate_unsupported_cpp_function(func, support[func.name]))
            lines.append("")

        lines.extend(self._generate_cpp_init(doc, support))
        lines.extend(
            [
                "",
                "} // namespace",
                "",
                f"NODE_API_MODULE({self.addon_name}, Init)",
                "",
            ]
        )
        return "\n".join(lines)

    def _generate_cpp_wrapper_base(self) -> list[str]:
        return [
            "template <typename WrapperT, typename InterfaceT>",
            "class DasInterfaceWrapperBase : public Napi::ObjectWrap<WrapperT> {",
            "public:",
            "    enum class OwnershipMode {",
            "        AdoptOwned,",
            "        BorrowAddRef,",
            "    };",
            "",
            "    explicit DasInterfaceWrapperBase(const Napi::CallbackInfo& info)",
            "        : Napi::ObjectWrap<WrapperT>(info) {}",
            "",
            "    static Napi::FunctionReference& Constructor() {",
            "        static Napi::FunctionReference constructor;",
            "        return constructor;",
            "    }",
            "",
            "    static void Register(Napi::Env env, const char* class_name) {",
            "        Napi::Function constructor = WrapperT::DefineClass(",
            "            env,",
            "            class_name,",
            "            {",
            "                WrapperT::InstanceMethod(\"dispose\", &WrapperT::DisposeMethod),",
            "            });",
            "        Constructor() = Napi::Persistent(constructor);",
            "        Constructor().SuppressDestruct();",
            "    }",
            "",
            "    static Napi::Object WrapAdopted(Napi::Env env, InterfaceT* raw) {",
            "        return Wrap(env, raw, OwnershipMode::AdoptOwned);",
            "    }",
            "",
            "    static Napi::Object WrapAdopted(",
            "        Napi::Env env,",
            "        DAS::DasPtr<InterfaceT> native) {",
            "        return Wrap(env, std::move(native));",
            "    }",
            "",
            "    static Napi::Object WrapBorrowed(Napi::Env env, InterfaceT* raw) {",
            "        return Wrap(env, raw, OwnershipMode::BorrowAddRef);",
            "    }",
            "",
            "    static InterfaceT* UnwrapHandle(",
            "        Napi::Env env,",
            "        const Napi::Value& value) {",
            "        if (!value.IsObject()) {",
            "            throw Napi::TypeError::New(env, \"DAS native wrapper handle expected\");",
            "        }",
            "        Napi::Object object = value.As<Napi::Object>();",
            "        if (!object.InstanceOf(Constructor().Value())) {",
            "            throw Napi::TypeError::New(env, \"DAS native wrapper handle expected\");",
            "        }",
            "        return WrapperT::Unwrap(object)->EnsureAlive(env);",
            "    }",
            "",
            "    InterfaceT* EnsureAlive(Napi::Env env) {",
            "        if (!native_) {",
            "            throw Napi::Error::New(env, \"DAS interface wrapper has been disposed\");",
            "        }",
            "        return native_.Get();",
            "    }",
            "",
            "    Napi::Value DisposeMethod(const Napi::CallbackInfo& info) {",
            "        Dispose();",
            "        return info.Env().Undefined();",
            "    }",
            "",
            "    void Dispose() noexcept { native_.Reset(); }",
            "",
            "    void Finalize(Napi::Env env) override {",
            "        (void)env;",
            "        Dispose();",
            "    }",
            "",
            "protected:",
            "    DAS::DasPtr<InterfaceT> native_;",
            "",
            "private:",
            "    static Napi::Object Wrap(",
            "        Napi::Env env,",
            "        InterfaceT* raw,",
            "        OwnershipMode ownership) {",
            "        if (ownership == OwnershipMode::AdoptOwned) {",
            "            return WrapAdopted(env, DAS::DasPtr<InterfaceT>::Attach(raw));",
            "        }",
            "        return Wrap(env, DAS::DasPtr<InterfaceT>(raw));",
            "    }",
            "",
            "    static Napi::Object Wrap(",
            "        Napi::Env env,",
            "        DAS::DasPtr<InterfaceT> native) {",
            "        if (!native) {",
            "            throw Napi::TypeError::New(env, \"DAS native pointer must not be null\");",
            "        }",
            "        Napi::Object object = Constructor().New({});",
            "        auto* wrapper = WrapperT::Unwrap(object);",
            "        wrapper->native_ = std::move(native);",
            "        return object;",
            "    }",
            "};",
        ]

    def _generate_cpp_wrapper_class(self, interface_name: str) -> list[str]:
        wrapper_name = _cpp_wrapper_name(interface_name)
        return [
            f"class {wrapper_name} final",
            f"    : public DasInterfaceWrapperBase<{wrapper_name}, {interface_name}> {{",
            "public:",
            f"    using Base = DasInterfaceWrapperBase<{wrapper_name}, {interface_name}>;",
            f"    explicit {wrapper_name}(const Napi::CallbackInfo& info)",
            "        : Base(info) {}",
            "};",
        ]

    def _generate_cpp_base_extractor(self, doc: IdlDocument) -> list[str]:
        lines = [
            "IDasBase* ExtractIDasBaseFromWrapper(",
            "    Napi::Env env,",
            "    const Napi::Value& value) {",
            "    if (!value.IsObject()) {",
            "        throw Napi::TypeError::New(env, \"DAS interface wrapper expected\");",
            "    }",
            "    Napi::Object object = value.As<Napi::Object>();",
        ]
        for interface_name in _wrapped_interface_names(doc):
            wrapper_name = _cpp_wrapper_name(interface_name)
            lines.extend(
                [
                    f"    if (object.InstanceOf({wrapper_name}::Constructor().Value())) {{",
                    f"        return static_cast<IDasBase*>({wrapper_name}::Unwrap(object)->EnsureAlive(env));",
                    "    }",
                ]
            )
        lines.extend(
            [
                "    throw Napi::TypeError::New(env, \"DAS interface wrapper expected\");",
                "}",
            ]
        )
        return lines

    def _generate_cpp_interface_helpers(
        self,
        interface: InterfaceDef,
        doc: IdlDocument,
    ) -> list[str]:
        lines: list[str] = []
        lines.extend(self._generate_cpp_dispose_function(interface.name))
        lines.append("")
        lines.extend(self._generate_cpp_from_function(interface))
        lines.append("")
        for method in interface.methods:
            js_name = public_js_name(method.name)
            wrapper_name = f"{_sanitize_identifier(interface.name)}_{_sanitize_identifier(js_name)}"
            lines.extend(
                [
                    f"Napi::Value {wrapper_name}(const Napi::CallbackInfo& info) {{",
                    "    Napi::Env env = info.Env();",
                    "    if (info.Length() < 1) {",
                    "        throw Napi::TypeError::New(env, \"DAS method expects a native wrapper handle\");",
                    "    }",
                    f"    auto* native = {_cpp_wrapper_name(interface.name)}::UnwrapHandle(env, info[0]);",
                    "    (void)native;",
                    "    const DasResult result = DAS_E_NO_IMPLEMENTATION;",
                    "    if (result < 0) {",
                    f'        ThrowDasException(env, result, "{interface.name}.{js_name} failed");',
                    "        return env.Undefined();",
                    "    }",
                ]
            )
            lines.extend(self._generate_cpp_method_success_return(method, doc))
            lines.append("}")
            lines.append("")
        return lines

    def _generate_cpp_dispose_function(self, interface_name: str) -> list[str]:
        wrapper_name = _cpp_wrapper_name(interface_name)
        function_name = f"{_sanitize_identifier(interface_name)}_dispose"
        return [
            f"Napi::Value {function_name}(const Napi::CallbackInfo& info) {{",
            "    Napi::Env env = info.Env();",
            "    if (info.Length() != 1 || !info[0].IsObject()) {",
            "        throw Napi::TypeError::New(env, \"dispose expects a native wrapper handle\");",
            "    }",
            f"    if (!info[0].As<Napi::Object>().InstanceOf({wrapper_name}::Constructor().Value())) {{",
            "        throw Napi::TypeError::New(env, \"dispose expects a native wrapper handle\");",
            "    }",
            f"    {wrapper_name}::Unwrap(info[0].As<Napi::Object>())->Dispose();",
            "    return env.Undefined();",
            "}",
        ]

    def _generate_cpp_from_function(self, interface: InterfaceDef) -> list[str]:
        interface_name = _sanitize_identifier(interface.name)
        wrapper_name = _cpp_wrapper_name(interface.name)
        return [
            f"Napi::Value {interface_name}_from(const Napi::CallbackInfo& info) {{",
            "    Napi::Env env = info.Env();",
            "    if (info.Length() != 1) {",
            "        throw Napi::TypeError::New(env, \"from(base) expects one argument\");",
            "    }",
            "    IDasBase* base = ExtractIDasBaseFromWrapper(env, info[0]);",
            "    void* cast_object = nullptr;",
            f"    const DasResult result = base->QueryInterface(DasIidOf<{interface.name}>(), &cast_object);",
            "    if (result < 0 || cast_object == nullptr) {",
            f'        ThrowDasException(env, result, "{interface.name}.from failed");',
            "        return env.Undefined();",
            "    }",
            f"    auto owned_target = DAS::DasPtr<{interface.name}>::Attach(static_cast<{interface.name}*>(cast_object));",
            f"    return {wrapper_name}::WrapAdopted(env, std::move(owned_target));",
            "}",
        ]

    def _generate_cpp_method_success_return(
        self,
        method: MethodDef,
        doc: IdlDocument,
    ) -> list[str]:
        del doc
        out_params = _method_out_params(method)
        if not out_params:
            return [
                "    return Napi::Number::New(env, static_cast<double>(result));",
            ]
        if len(out_params) == 1:
            return self._generate_cpp_placeholder_value(out_params[0], "    ")

        lines = ["    Napi::Object output = Napi::Object::New(env);"]
        for param in out_params:
            field_name = clean_out_field_name(param.name)
            value_lines = self._generate_cpp_placeholder_value(param, "    ")
            value_expr = value_lines[-1].strip()
            if value_expr.startswith("return "):
                value_expr = value_expr[len("return "):].rstrip(";")
            lines.extend(value_lines[:-1])
            lines.append(f'    output.Set("{field_name}", {value_expr});')
        lines.append("    return output;")
        return lines

    def _generate_cpp_placeholder_value(
        self,
        param: ParameterDef,
        indent: str,
    ) -> list[str]:
        value_type = _value_type_for_out_param(param)
        if value_type.type_kind == TypeKind.STRUCT:
            object_name = f"{_sanitize_identifier(param.name)}_object"
            lines = [f"{indent}Napi::Object {object_name} = Napi::Object::New(env);"]
            for field_name in ("width", "height"):
                lines.append(
                    f'{indent}{object_name}.Set("{field_name}", Napi::Number::New(env, 0));'
                )
            lines.append(f"{indent}return {object_name};")
            return lines
        if _is_binary_buffer_interface(value_type):
            return [f"{indent}return Napi::Buffer<unsigned char>::New(env, nullptr, 0);"]
        if value_type.type_kind == TypeKind.INTERFACE:
            return [f"{indent}return env.Undefined();"]

        mapped = _map_type(value_type)
        if mapped is None:
            return [f"{indent}return env.Undefined();"]
        if mapped.ts_type == "bigint":
            return [f"{indent}return Napi::BigInt::New(env, static_cast<uint64_t>(0));"]
        if mapped.ts_type == "boolean":
            return [f"{indent}return Napi::Boolean::New(env, false);"]
        return [f"{indent}return Napi::Number::New(env, 0);"]

    def _generate_supported_cpp_function(self, func: ModuleFunctionDef) -> list[str]:
        wrapper_name = f"Wrap_{_sanitize_identifier(func.name)}"
        lines = [
            f"Napi::Value {wrapper_name}(const Napi::CallbackInfo& info) {{",
            "    Napi::Env env = info.Env();",
            f"    if (info.Length() != {len(func.parameters)}) {{",
            "        throw Napi::TypeError::New(",
            "            env,",
            f'            "{func.name} expects {len(func.parameters)} argument(s)");',
            "    }",
        ]
        call_args: list[str] = []
        cleanup_lines: list[str] = []

        for index, param in enumerate(func.parameters):
            mapped = _map_type(param.type_info)
            if mapped is None:
                raise AssertionError(f"supported function has unmapped param: {func.name}")
            lines.extend(self._generate_param_conversion(index, param, mapped, cleanup_lines))
            call_args.append(self._call_argument(param, mapped))

        ret = _map_type(func.return_type, for_return=True)
        if ret is None:
            raise AssertionError(f"supported function has unmapped return: {func.name}")

        call = f"::{func.name}({', '.join(call_args)})"
        if ret.category == "void":
            lines.append(f"    {call};")
            lines.extend(cleanup_lines)
            lines.append("    return env.Undefined();")
        else:
            lines.append(f"    const {ret.cpp_type} result = {call};")
            lines.extend(cleanup_lines)
            lines.append(f"    return {self._cpp_return_expression('result', ret)};")

        lines.append("}")
        return lines

    def _generate_param_conversion(
        self,
        index: int,
        param: ParameterDef,
        mapped: _MappedType,
        cleanup_lines: list[str],
    ) -> list[str]:
        name = _sanitize_identifier(param.name)
        type_error = f"{param.name} must be {mapped.js_check}"
        lines = [
            f"    if (!info[{index}].Is{self._napi_type_check(mapped)}()) {{",
            "        throw Napi::TypeError::New(",
            "            env,",
            f'            "{type_error}");',
            "    }",
        ]
        if mapped.category == "utf8_string":
            lines.append(
                f"    const std::string {name}_storage = info[{index}].As<Napi::String>().Utf8Value();"
            )
            lines.append(f"    const char* {name}_value = {name}_storage.c_str();")
        elif mapped.category == "readonly_string":
            lines.append(
                f"    const std::string {name}_storage = info[{index}].As<Napi::String>().Utf8Value();"
            )
            lines.append(f"    IDasReadOnlyString* {name}_value = nullptr;")
            lines.append(
                f"    const DasResult {name}_create_result = CreateIDasReadOnlyStringFromUtf8("
            )
            lines.append(f"        {name}_storage.c_str(), &{name}_value);")
            lines.append(
                f"    if ({name}_create_result < 0 || {name}_value == nullptr) {{"
            )
            lines.append(
                "        throw Napi::Error::New(env, \"failed to create DAS string\");"
            )
            lines.append("    }")
            cleanup_lines.append(f"    {name}_value->Release();")
        elif mapped.category == "das_guid":
            lines.append(f"    const DasGuid {name}_value = ReadDasGuid(env, info[{index}]);")
        elif mapped.category == "int64":
            lines.append(f"    bool {name}_lossless = false;")
            lines.append(
                f"    const int64_t {name}_raw = info[{index}].As<Napi::BigInt>().Int64Value(&{name}_lossless);"
            )
            lines.append(f"    if (!{name}_lossless) {{")
            lines.append(
                "        throw Napi::RangeError::New(env, \"bigint value is out of int64 range\");"
            )
            lines.append("    }")
            lines.append(
                f"    const {mapped.cpp_type} {name}_value = static_cast<{mapped.cpp_type}>({name}_raw);"
            )
        elif mapped.category == "uint64":
            lines.append(f"    bool {name}_lossless = false;")
            lines.append(
                f"    const uint64_t {name}_raw = info[{index}].As<Napi::BigInt>().Uint64Value(&{name}_lossless);"
            )
            lines.append(f"    if (!{name}_lossless) {{")
            lines.append(
                "        throw Napi::RangeError::New(env, \"bigint value is out of uint64 range\");"
            )
            lines.append("    }")
            lines.append(
                f"    const {mapped.cpp_type} {name}_value = static_cast<{mapped.cpp_type}>({name}_raw);"
            )
        elif mapped.category in {"int32", "enum"}:
            lines.append(
                f"    const {mapped.cpp_type} {name}_value = static_cast<{mapped.cpp_type}>("
            )
            lines.append(f"        info[{index}].As<Napi::Number>().Int32Value());")
        elif mapped.category == "uint32":
            lines.append(
                f"    const {mapped.cpp_type} {name}_value = static_cast<{mapped.cpp_type}>("
            )
            lines.append(f"        info[{index}].As<Napi::Number>().Uint32Value());")
        elif mapped.category == "number":
            lines.append(
                f"    const {mapped.cpp_type} {name}_value = static_cast<{mapped.cpp_type}>("
            )
            lines.append(f"        info[{index}].As<Napi::Number>().DoubleValue());")
        elif mapped.category == "boolean":
            if type_simple_name(param.type_info) == "DasBool":
                lines.append(
                    f"    const DasBool {name}_value = info[{index}].As<Napi::Boolean>().Value()"
                )
                lines.append("        ? DAS_TRUE")
                lines.append("        : DAS_FALSE;")
            else:
                lines.append(
                    f"    const bool {name}_value = info[{index}].As<Napi::Boolean>().Value();"
                )
        else:
            raise AssertionError(f"unhandled mapped category {mapped.category}")
        return lines

    def _call_argument(self, param: ParameterDef, mapped: _MappedType) -> str:
        del mapped
        return f"{_sanitize_identifier(param.name)}_value"

    def _cpp_return_expression(self, value_name: str, mapped: _MappedType) -> str:
        if mapped.category in {"das_result", "int32", "uint32", "number", "enum"}:
            return f"Napi::Number::New(env, static_cast<double>({value_name}))"
        if mapped.category == "boolean":
            return f"Napi::Boolean::New(env, {value_name} != 0)"
        if mapped.category == "int64":
            return f"Napi::BigInt::New(env, static_cast<int64_t>({value_name}))"
        if mapped.category == "uint64":
            return f"Napi::BigInt::New(env, static_cast<uint64_t>({value_name}))"
        if mapped.category == "das_guid":
            return f"WriteDasGuid(env, {value_name})"
        raise AssertionError(f"unhandled return category {mapped.category}")

    def _napi_type_check(self, mapped: _MappedType) -> str:
        if mapped.js_check == "string":
            return "String"
        if mapped.js_check == "number":
            return "Number"
        if mapped.js_check == "boolean":
            return "Boolean"
        if mapped.js_check == "bigint":
            return "BigInt"
        raise AssertionError(f"unhandled JS check {mapped.js_check}")

    def _generate_unsupported_cpp_function(
        self,
        func: ModuleFunctionDef,
        support: FunctionSupport,
    ) -> list[str]:
        wrapper_name = f"Wrap_{_sanitize_identifier(func.name)}"
        reason = _escape_cpp_string(support.reason)
        return [
            f"Napi::Value {wrapper_name}(const Napi::CallbackInfo& info) {{",
            f'    return MakeUnsupported(info, "{func.name}", "{reason}");',
            "}",
        ]

    def _generate_cpp_init(
        self,
        doc: IdlDocument,
        support: dict[str, FunctionSupport],
    ) -> list[str]:
        lines = [
            "Napi::Object Init(Napi::Env env, Napi::Object exports) {",
        ]
        for interface_name in _wrapped_interface_names(doc):
            lines.append(
                f'    {_cpp_wrapper_name(interface_name)}::Register(env, "{interface_name}");'
            )
        lines.extend(
            [
            "",
            "    Napi::Object result_constants = Napi::Object::New(env);",
            ]
        )
        for error_code in doc.error_codes:
            for value in error_code.values:
                lines.append(
                    f'    result_constants.Set("{value.name}", Napi::Number::New(env, {value.value}));'
                )
        lines.append('    exports.Set("DasResult", result_constants);')
        lines.append("")

        for enum in doc.enums:
            lines.append(f"    Napi::Object {enum.name}_constants = Napi::Object::New(env);")
            for value in enum.values:
                lines.append(
                    f'    {enum.name}_constants.Set("{value.name}", Napi::Number::New(env, {value.value}));'
                )
            lines.append(f'    exports.Set("{enum.name}", {enum.name}_constants);')
            lines.append("")

        lines.append('    exports.Set("guid", Napi::Function::New(env, Guid));')
        for interface_name in _wrapped_interface_names(doc):
            dispose_name = f"{_sanitize_identifier(interface_name)}_dispose"
            lines.append(
                f'    exports.Set("{dispose_name}", Napi::Function::New(env, {dispose_name}));'
            )
        for interface in doc.interfaces:
            from_name = f"{_sanitize_identifier(interface.name)}_from"
            lines.append(
                f'    exports.Set("{from_name}", Napi::Function::New(env, {from_name}));'
            )
        for interface in doc.interfaces:
            for method in interface.methods:
                js_name = public_js_name(method.name)
                wrapper_name = f"{_sanitize_identifier(interface.name)}_{_sanitize_identifier(js_name)}"
                lines.append(
                    f'    exports.Set("{wrapper_name}", Napi::Function::New(env, {wrapper_name}));'
                )
        for func in _iter_module_functions(doc):
            wrapper_name = f"Wrap_{_sanitize_identifier(func.name)}"
            if func.name not in support:
                raise AssertionError(f"missing support table entry for {func.name}")
            lines.append(
                f'    exports.Set("{func.name}", Napi::Function::New(env, {wrapper_name}));'
            )
        lines.append("    return exports;")
        lines.append("}")
        return lines

    def _generate_dts(
        self,
        doc: IdlDocument,
        support: dict[str, FunctionSupport],
    ) -> str:
        lines = [
            "// DAS Node/NAPI declarations (auto-generated - DO NOT MODIFY)",
            f"// Package: {self.package_name}",
            '/// <reference types="node" />',
            "",
            "export type DasResult = number & { readonly __dasResultBrand?: unique symbol };",
            "export const DasResult: Readonly<{",
        ]
        for error_code in doc.error_codes:
            for value in error_code.values:
                lines.append(f"  {value.name}: {value.value};")
        lines.extend(["}>;", ""])

        lines.extend(
            [
                "export class DasException extends Error {",
                "  readonly result: DasResult;",
                "  readonly code: DasResult;",
                "  constructor(result: DasResult, message?: string);",
                "}",
                "",
            ]
        )

        for enum in doc.enums:
            lines.append(f"export const {enum.name}: Readonly<{{")
            for value in enum.values:
                lines.append(f"  {value.name}: {value.value};")
            lines.append("}>;")
            lines.append("")

        for struct in doc.structs:
            lines.extend(self._generate_dts_struct(struct))
            lines.append("")

        lines.extend(
            [
                "export type DasGuid = string & { readonly __dasGuidBrand: unique symbol };",
                "export function guid(value: string): DasGuid;",
                "",
            ]
        )
        if not any(interface.name == "IDasBase" for interface in doc.interfaces):
            lines.extend(
                [
                    "export class IDasBase {",
                    "  dispose(): void;",
                    "}",
                    "",
                ]
            )
        for interface in doc.interfaces:
            lines.extend(self._generate_dts_interface(interface))
            lines.append("")
            lines.extend(self._generate_dts_director(interface))
            lines.append("")

        for func in _iter_module_functions(doc):
            if support[func.name].supported:
                lines.append(self._dts_supported_signature(func))
            else:
                reason = support[func.name].reason
                lines.append(f"/** Unsupported: {reason} */")
                lines.append(f"export function {func.name}(...args: never[]): never;")
            lines.append("")
        return "\n".join(lines)

    def _generate_dts_struct(self, struct: StructDef) -> list[str]:
        lines = [f"export interface {struct.name} {{"]
        for field in struct.fields:
            lines.append(f"  {public_js_name(field.name)}: {_ts_type_for_struct_field(field.type_name)};")
        lines.append("}")
        return lines

    def _generate_dts_interface(self, interface: InterfaceDef) -> list[str]:
        base = interface.base_interface or "IDasBase"
        extends = f" extends {base}" if interface.name != "IDasBase" else ""
        lines = [f"export class {interface.name}{extends} {{"]
        lines.append(f"  static from(base: IDasBase): {interface.name};")
        lines.append("  dispose(): void;")
        for method in interface.methods:
            js_name = public_js_name(method.name)
            lines.append(
                f"  {js_name}({_method_ts_params(method)}): {_method_ts_return(method)};"
            )
        lines.append("}")
        return lines

    def _generate_dts_director(self, interface: InterfaceDef) -> list[str]:
        director_name = self._director_name(interface.name)
        callbacks_name = f"{director_name}Callbacks"
        lines = [f"export interface {callbacks_name} {{"]
        for method in interface.methods:
            js_name = public_js_name(method.name)
            lines.append(
                f"  {js_name}?: ({_method_ts_params(method)}) => {_method_ts_return(method)};"
            )
        lines.append("}")
        lines.append(f"export class {director_name} extends {interface.name} {{")
        lines.append(f"  constructor(callbacks: {callbacks_name});")
        lines.append("}")
        return lines

    def _dts_supported_signature(self, func: ModuleFunctionDef) -> str:
        params = []
        for param in func.parameters:
            mapped = _map_type(param.type_info)
            if mapped is None:
                raise AssertionError(f"unmapped dts param: {func.name}")
            params.append(f"{param.name}: {mapped.ts_type}")
        ret = _map_type(func.return_type, for_return=True)
        if ret is None:
            raise AssertionError(f"unmapped dts return: {func.name}")
        ret_type = "void" if ret.category == "void" else ret.ts_type
        return f"export function {func.name}({', '.join(params)}): {ret_type};"

    def _generate_js(
        self,
        doc: IdlDocument,
        support: dict[str, FunctionSupport],
    ) -> str:
        lines = [
            "'use strict';",
            "",
            "const path = require('node:path');",
            "let native;",
            "try {",
            f"  native = require(path.join(__dirname, '{self.addon_name}.node'));",
            "} catch (error) {",
            f"  const wrapped = new Error('Failed to load DAS native addon {self.addon_name}.node');",
            "  wrapped.cause = error;",
            "  throw wrapped;",
            "}",
            "",
            "function unsupported(name, reason) {",
            "  return function unsupportedCapability() {",
            "    throw new Error(`${name} is unsupported: ${reason}`);",
            "  };",
            "}",
            "",
            "function guid(value) {",
            "  if (typeof value !== 'string') {",
            "    throw new TypeError('guid(value) expects a string');",
            "  }",
            "  return native.guid(value);",
            "}",
            "",
            "class DasException extends Error {",
            "  constructor(result, message) {",
            "    super(message || `DAS call failed with result ${result}`);",
            "    this.name = 'DasException';",
            "    this.result = result;",
            "    this.code = result;",
            "  }",
            "}",
            "",
        ]
        if not any(interface.name == "IDasBase" for interface in doc.interfaces):
            lines.extend(self._generate_js_base_class())
            lines.append("")
        for interface in doc.interfaces:
            lines.extend(self._generate_js_interface(interface))
            lines.append("")
            lines.extend(self._generate_js_director(interface))
            lines.append("")

        lines.extend(
            [
                "module.exports = {",
                "  DasResult: native.DasResult,",
                "  DasException,",
            ]
        )
        for enum in doc.enums:
            lines.append(f"  {enum.name}: native.{enum.name},")
        lines.append("  guid,")
        if not any(interface.name == "IDasBase" for interface in doc.interfaces):
            lines.append("  IDasBase,")
        for interface in doc.interfaces:
            director_name = self._director_name(interface.name)
            lines.append(f"  {interface.name},")
            lines.append(f"  {director_name},")
        for func in _iter_module_functions(doc):
            if support[func.name].supported:
                lines.append(f"  {func.name}: native.{func.name},")
            else:
                reason = _escape_js_string(support[func.name].reason)
                lines.append(
                    f"  {func.name}: unsupported('{func.name}', '{reason}'),"
                )
        lines.append("};")
        lines.append("")
        return "\n".join(lines)

    def _generate_js_base_class(self) -> list[str]:
        return [
            "class IDasBase {",
            "  constructor(nativeHandle) {",
            "    this._native = nativeHandle;",
            "  }",
            "  _ensureAlive() {",
            "    if (!this._native) {",
            "      throw new Error('DAS interface wrapper has been disposed');",
            "    }",
            "    return this._native;",
            "  }",
            "  dispose() {",
            "    if (this._native && native.IDasBase_dispose) {",
            "      native.IDasBase_dispose(this._native);",
            "    }",
            "    this._native = null;",
            "  }",
            "}",
        ]

    def _generate_js_interface(self, interface: InterfaceDef) -> list[str]:
        base = interface.base_interface or "IDasBase"
        extends = f" extends {base}" if interface.name != "IDasBase" else ""
        lines = [f"class {interface.name}{extends} {{"]
        lines.extend(
            [
                "  constructor(nativeHandle) {",
            ]
        )
        if interface.name != "IDasBase":
            lines.append("    super(nativeHandle);")
        else:
            lines.append("    this._native = nativeHandle;")
        lines.extend(
            [
                "  }",
            ]
        )
        if interface.name == "IDasBase":
            lines.extend(
                [
                    "  _ensureAlive() {",
                    "    if (!this._native) {",
                    "      throw new Error('DAS interface wrapper has been disposed');",
                    "    }",
                    "    return this._native;",
                    "  }",
                ]
            )
        lines.extend(
            [
                "  dispose() {",
                f"    if (this._native && native.{interface.name}_dispose) {{",
                f"      native.{interface.name}_dispose(this._native);",
                "    }",
                "    this._native = null;",
                "  }",
                "  static from(base) {",
                "    if (!base || typeof base !== 'object' || !base._native) {",
                "      throw new Error('DAS interface wrapper has been disposed');",
                "    }",
                f"    return new {interface.name}(native.{interface.name}_from(base._native));",
                "  }",
            ]
        )
        for method in interface.methods:
            js_name = public_js_name(method.name)
            native_name = f"{interface.name}_{js_name}"
            lines.extend(
                [
                    f"  {js_name}(...args) {{",
                    f"    return native.{native_name}(this._ensureAlive(), ...args);",
                    "  }",
                ]
            )
        lines.append("}")
        return lines

    def _generate_js_director(self, interface: InterfaceDef) -> list[str]:
        director_name = self._director_name(interface.name)
        lines = [f"class {director_name} extends {interface.name} {{"]
        lines.extend(
            [
                "  constructor(callbacks) {",
                "    if (callbacks === null || typeof callbacks !== 'object') {",
                f"      throw new TypeError('{director_name} expects a callback object');",
                "    }",
                f"    super(native.create{director_name} ? native.create{director_name}(callbacks) : callbacks);",
                "    this.callbacks = callbacks;",
                "  }",
                "}",
            ]
        )
        return lines

    def _director_name(self, interface_name: str) -> str:
        if interface_name.startswith("IDas"):
            return f"INapiDas{interface_name[4:]}"
        if interface_name.startswith("I"):
            return f"INapi{interface_name[1:]}"
        return f"INapi{interface_name}"


def generate_napi_artifacts(
    doc: IdlDocument,
    *,
    package_name: str,
    addon_name: str,
    idl_header_names: Sequence[str] | None = None,
) -> NapiArtifacts:
    return NapiGenerator(
        package_name=package_name,
        addon_name=addon_name,
        idl_header_names=idl_header_names,
    ).generate(doc)
