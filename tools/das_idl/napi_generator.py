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


def _idl_namespaces(doc: IdlDocument) -> list[str]:
    namespaces = {
        item.namespace
        for collection in (doc.interfaces, doc.structs, doc.enums, doc.error_codes)
        for item in collection
        if item.namespace
    }
    namespaces.update(module.namespace for module in doc.modules if module.namespace)
    return sorted(namespaces)


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


def _find_struct(doc: IdlDocument, type_info: TypeInfo) -> StructDef | None:
    name = type_simple_name(type_info)
    namespace = type_info.resolved_namespace
    for struct in doc.structs:
        if struct.name != name:
            continue
        if not namespace or struct.namespace == namespace:
            return struct
    return None


def _cpp_storage_type(type_info: TypeInfo) -> str:
    return _cpp_type(
        replace(
            type_info,
            is_const=False,
            is_reference=False,
        )
    )


def _ts_type_for_struct_field(type_name: str, doc: IdlDocument | None = None) -> str:
    if type_name in _SIGNED_64_TYPES or type_name in _UNSIGNED_64_TYPES:
        return "bigint"
    if type_name in _SIGNED_32_TYPES or type_name in _UNSIGNED_32_TYPES:
        return "number"
    if type_name in _FLOAT_TYPES:
        return "number"
    if type_name in _BOOL_TYPES:
        return "boolean"
    if doc is not None and _find_struct(doc, TypeInfo(type_name)) is not None:
        return type_simple_name(TypeInfo(type_name))
    return "unknown"


def _ts_type_for_out_param(param: ParameterDef) -> str:
    value_type = _value_type_for_out_param(param)
    if _is_binary_buffer_interface(value_type):
        return "Buffer"
    mapped = _map_type(value_type)
    if mapped is not None:
        return mapped.ts_type
    if value_type.type_kind == TypeKind.STRUCT:
        return type_simple_name(value_type)
    if value_type.type_kind == TypeKind.INTERFACE:
        return type_simple_name(value_type)
    return "unknown"


def _ts_type_for_in_param(param: ParameterDef) -> str:
    mapped = _map_type(param.type_info)
    if mapped is not None:
        return mapped.ts_type
    if param.type_info.type_kind == TypeKind.STRUCT:
        return type_simple_name(param.type_info)
    if (
        param.type_info.type_kind == TypeKind.INTERFACE
        and param.type_info.is_pointer
        and param.type_info.pointer_level == 1
    ):
        return type_simple_name(param.type_info)
    return "unknown"


def _method_in_params(method: MethodDef) -> list[ParameterDef]:
    return [param for param in method.parameters if param.direction == ParamDirection.IN]


def _method_out_params(method: MethodDef) -> list[ParameterDef]:
    return [param for param in method.parameters if param.direction == ParamDirection.OUT]


def _method_ts_params(method: MethodDef) -> str:
    params = []
    for param in _method_in_params(method):
        params.append(f"{public_js_name(param.name)}: {_ts_type_for_in_param(param)}")
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


def _doc_needs_binary_buffer_conversion(doc: IdlDocument) -> bool:
    for interface in doc.interfaces:
        for method in _interface_declared_methods(interface):
            for param in _method_out_params(method):
                if _is_binary_buffer_interface(_value_type_for_out_param(param)):
                    return True
    return False


def _doc_has_error_code(doc: IdlDocument, name: str) -> bool:
    for error_code in doc.error_codes:
        for value in error_code.values:
            if value.name == name:
                return True
    return False


def _interface_by_name(doc: IdlDocument) -> dict[str, InterfaceDef]:
    return {interface.name: interface for interface in doc.interfaces}


def _interface_chain(interface: InterfaceDef, doc: IdlDocument) -> list[str]:
    interfaces = _interface_by_name(doc)
    chain = [interface.name]
    base_name = interface.base_interface
    while base_name and base_name != "IDasBase":
        chain.append(base_name)
        base = interfaces.get(base_name)
        if base is None:
            break
        base_name = base.base_interface
    return chain


def _interfaces_base_first(doc: IdlDocument) -> list[InterfaceDef]:
    interfaces = _interface_by_name(doc)
    ordered: list[InterfaceDef] = []
    visiting: set[str] = set()
    emitted: set[str] = set()

    def visit(interface: InterfaceDef) -> None:
        if interface.name in emitted:
            return
        if interface.name in visiting:
            return
        visiting.add(interface.name)
        base_name = interface.base_interface
        if base_name and base_name != "IDasBase":
            base = interfaces.get(base_name)
            if base is not None:
                visit(base)
        visiting.remove(interface.name)
        emitted.add(interface.name)
        ordered.append(interface)

    for interface in doc.interfaces:
        visit(interface)
    return ordered


def _property_accessor_methods(interface: InterfaceDef) -> list[MethodDef]:
    methods: list[MethodDef] = []
    for prop in interface.properties:
        value_type = replace(prop.type_info, is_const=False, is_reference=False)
        if value_type.type_kind == TypeKind.INTERFACE and not value_type.is_pointer:
            value_type = replace(value_type, is_pointer=True, pointer_level=1)
        if prop.has_getter:
            out_type = replace(
                value_type,
                is_pointer=True,
                pointer_level=value_type.pointer_level + 1,
            )
            methods.append(
                MethodDef(
                    name=f"Get{prop.name}",
                    return_type=TypeInfo("DasResult"),
                    parameters=[
                        ParameterDef(
                            name="p_out",
                            type_info=out_type,
                            direction=ParamDirection.OUT,
                        )
                    ],
                    namespace=interface.namespace,
                )
            )
        if prop.has_setter:
            methods.append(
                MethodDef(
                    name=f"Set{prop.name}",
                    return_type=TypeInfo("DasResult"),
                    parameters=[
                        ParameterDef(
                            name="value",
                            type_info=value_type,
                            direction=ParamDirection.IN,
                        )
                    ],
                    namespace=interface.namespace,
                )
            )
    return methods


def _interface_declared_methods(interface: InterfaceDef) -> list[MethodDef]:
    return [*interface.methods, *_property_accessor_methods(interface)]


def _interface_methods_including_bases(
    interface: InterfaceDef,
    doc: IdlDocument,
) -> list[MethodDef]:
    interfaces = _interface_by_name(doc)
    methods: list[MethodDef] = []
    for interface_name in reversed(_interface_chain(interface, doc)):
        current = interfaces.get(interface_name)
        if current is not None:
            methods.extend(_interface_declared_methods(current))
    return methods


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
            "#include <atomic>",
            "#include <condition_variable>",
            "#include <cstdint>",
            "#include <cstdio>",
            "#include <filesystem>",
            "#include <functional>",
            "#include <fstream>",
            "#include <iterator>",
            "#include <limits>",
            "#include <memory>",
            "#include <mutex>",
            "#include <string>",
            "#include <string_view>",
            "#include <thread>",
            "#include <utility>",
            "#include <vector>",
            "",
            '#include "das/Core/IPC/Host/HostCommandHandlers.h"',
            '#include "das/Core/IPC/Host/IIpcContext.h"',
            '#include "das/Core/IPC/IpcErrors.h"',
            '#include "das/IDasBase.h"',
            '#include "das/DasPtr.hpp"',
            '#include "das/DasString.hpp"',
            '#include "das/DasApi.h"',
            '#include "das/Utils/DasJsonCore.h"',
            '#include "das/_autogen/idl/abi/IDasPluginPackage.h"',
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
            ]
        )
        for namespace in _idl_namespaces(doc):
            lines.append(f"using namespace {namespace};")
        if _idl_namespaces(doc):
            lines.append("")
        lines.extend(
            [
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
                "Napi::Value ConvertDasReadOnlyStringToString(",
                "    Napi::Env env,",
                "    IDasReadOnlyString* value) {",
                "    if (value == nullptr) {",
                "        throw Napi::TypeError::New(env, \"DAS string pointer must not be null\");",
                "    }",
                "    const char* utf8 = nullptr;",
                "    const DasResult result = value->GetUtf8(&utf8);",
                "    if (result < 0 || utf8 == nullptr) {",
                "        ThrowDasException(env, result, \"IDasReadOnlyString.GetUtf8 failed\");",
                "        return env.Undefined();",
                "    }",
                "    return Napi::String::New(env, utf8);",
                "}",
                "",
            ]
        )

        lines.extend(self._generate_cpp_host_bootstrap())

        if _doc_needs_binary_buffer_conversion(doc):
            lines.extend(
                [
                    "enum class BinaryBufferOwnershipMode {",
                    "    AdoptOwned,",
                    "    BorrowAddRef,",
                    "};",
                    "",
                    "struct BinaryBufferViewHolder {",
                    "    explicit BinaryBufferViewHolder(",
                    "        DAS::DasPtr<IDasBinaryBuffer> in_buffer)",
                    "        : buffer(std::move(in_buffer)) {}",
                    "",
                    "    DAS::DasPtr<IDasBinaryBuffer> buffer;",
                    "};",
                    "",
                    "DAS::DasPtr<IDasBinaryBuffer> HoldIDasBinaryBuffer(",
                    "    IDasBinaryBuffer* raw,",
                    "    BinaryBufferOwnershipMode ownership) {",
                    "    if (raw == nullptr) {",
                    "        return {};",
                    "    }",
                    "    if (ownership == BinaryBufferOwnershipMode::AdoptOwned) {",
                    "        return DAS::DasPtr<IDasBinaryBuffer>::Attach(raw);",
                    "    }",
                    "    return DAS::DasPtr<IDasBinaryBuffer>(raw);",
                    "}",
                    "",
                    "Napi::Value ConvertIDasBinaryBufferToBuffer(",
                    "    Napi::Env env,",
                    "    DAS::DasPtr<IDasBinaryBuffer> buffer) {",
                    "    if (!buffer) {",
                    "        throw Napi::TypeError::New(env, \"IDasBinaryBuffer pointer must not be null\");",
                    "    }",
                    "    uint64_t byte_size = 0;",
                    "    const DasResult size_result = buffer->GetSize(&byte_size);",
                    "    if (size_result < 0) {",
                    "        ThrowDasException(env, size_result, \"IDasBinaryBuffer.GetSize failed\");",
                    "        return env.Undefined();",
                    "    }",
                    "    if (byte_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {",
                    "        throw Napi::RangeError::New(env, \"IDasBinaryBuffer size exceeds host size_t range\");",
                    "    }",
                    "    unsigned char* data = nullptr;",
                    "    const DasResult data_result = buffer->GetData(&data);",
                    "    if (data_result < 0) {",
                    "        ThrowDasException(env, data_result, \"IDasBinaryBuffer.GetData failed\");",
                    "        return env.Undefined();",
                    "    }",
                    "    if (data == nullptr) {",
                    "        throw Napi::TypeError::New(env, \"IDasBinaryBuffer data pointer must not be null\");",
                    "    }",
                    "    auto holder = std::make_unique<BinaryBufferViewHolder>(std::move(buffer));",
                    "    auto* holder_raw = holder.get();",
                    "    auto node_buffer = Napi::Buffer<unsigned char>::New(",
                    "        env,",
                    "        data,",
                    "        static_cast<size_t>(byte_size),",
                    "        [](",
                    "            Napi::Env finalizer_env,",
                    "            unsigned char* finalizer_data,",
                    "            BinaryBufferViewHolder* finalizer_holder) {",
                    "            (void)finalizer_env;",
                    "            (void)finalizer_data;",
                    "            delete finalizer_holder;",
                    "        },",
                    "        holder_raw);",
                    "    holder.release();",
                    "    return node_buffer;",
                    "}",
                    "",
                    "Napi::Value ConvertIDasBinaryBufferToBuffer(",
                    "    Napi::Env env,",
                    "    IDasBinaryBuffer* raw,",
                    "    BinaryBufferOwnershipMode ownership) {",
                    "    return ConvertIDasBinaryBufferToBuffer(",
                    "        env,",
                    "        HoldIDasBinaryBuffer(raw, ownership));",
                    "}",
                    "",
                    "class NapiDirectorBinaryBuffer final : public IDasBinaryBuffer {",
                    "public:",
                    "    explicit NapiDirectorBinaryBuffer(Napi::Buffer<unsigned char> buffer)",
                    "        : data_(buffer.Data(), buffer.Data() + buffer.Length()) {}",
                    "",
                    "    uint32_t AddRef() override {",
                    "        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;",
                    "    }",
                    "",
                    "    uint32_t Release() override {",
                    "        const uint32_t count = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;",
                    "        if (count == 0) {",
                    "            delete this;",
                    "        }",
                    "        return count;",
                    "    }",
                    "",
                    "    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override {",
                    "        if (pp_object == nullptr) {",
                    "            return DAS_E_INVALID_POINTER;",
                    "        }",
                    "        if (iid == DasIidOf<IDasBinaryBuffer>()) {",
                    "            *pp_object = static_cast<IDasBinaryBuffer*>(this);",
                    "            AddRef();",
                    "            return DAS_S_OK;",
                    "        }",
                    "        if (iid == DAS_IID_BASE) {",
                    "            *pp_object = static_cast<IDasBase*>(this);",
                    "            AddRef();",
                    "            return DAS_S_OK;",
                    "        }",
                    "        *pp_object = nullptr;",
                    "        return DAS_E_NO_INTERFACE;",
                    "    }",
                    "",
                    "    DasResult GetData(unsigned char** pp_out_data) override {",
                    "        if (pp_out_data == nullptr) {",
                    "            return DAS_E_INVALID_POINTER;",
                    "        }",
                    "        *pp_out_data = data_.data();",
                    "        return DAS_S_OK;",
                    "    }",
                    "",
                    "    DasResult GetSize(uint64_t* p_out_size) override {",
                    "        if (p_out_size == nullptr) {",
                    "            return DAS_E_INVALID_POINTER;",
                    "        }",
                    "        *p_out_size = static_cast<uint64_t>(data_.size());",
                    "        return DAS_S_OK;",
                    "    }",
                    "",
                    "private:",
                    "    std::atomic<uint32_t> ref_count_{1};",
                    "    std::vector<unsigned char> data_;",
                    "};",
                    "",
                ]
            )

        lines.extend(self._generate_cpp_wrapper_base())
        lines.append("")

        for interface_name in _wrapped_interface_names(doc):
            lines.extend(self._generate_cpp_wrapper_class(interface_name))
            lines.append("")

        lines.extend(self._generate_cpp_director_base(doc))
        lines.append("")

        for interface in doc.interfaces:
            if interface.name == "IDasBase":
                continue
            lines.extend(self._generate_cpp_director_class(interface, doc))
            lines.append("")
            lines.extend(self._generate_cpp_director_create_function(interface))
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

    def _generate_cpp_host_bootstrap(self) -> list[str]:
        return [
            "struct NodeHostBootstrapPaths {",
            "    std::filesystem::path package_root;",
            "    std::filesystem::path wrapper_path;",
            "    std::filesystem::path addon_path;",
            "};",
            "",
            "class MinimalNodePluginPackage final",
            "    : public Das::PluginInterface::IDasPluginPackage {",
            "public:",
            "    explicit MinimalNodePluginPackage(NodeHostBootstrapPaths paths)",
            "        : paths_(std::move(paths)) {}",
            "",
            "    uint32_t AddRef() override {",
            "        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;",
            "    }",
            "",
            "    uint32_t Release() override {",
            "        const uint32_t count =",
            "            ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;",
            "        if (count == 0) {",
            "            delete this;",
            "        }",
            "        return count;",
            "    }",
            "",
            "    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override {",
            "        if (pp_object == nullptr) {",
            "            return DAS_E_INVALID_POINTER;",
            "        }",
            "        if (iid == DAS_IID_PLUGIN_PACKAGE) {",
            "            *pp_object = static_cast<Das::PluginInterface::IDasPluginPackage*>(this);",
            "            AddRef();",
            "            return DAS_S_OK;",
            "        }",
            "        if (iid == DAS_IID_BASE) {",
            "            *pp_object = static_cast<IDasBase*>(this);",
            "            AddRef();",
            "            return DAS_S_OK;",
            "        }",
            "        *pp_object = nullptr;",
            "        return DAS_E_NO_INTERFACE;",
            "    }",
            "",
            "    DasResult EnumFeature(",
            "        uint64_t index,",
            "        Das::PluginInterface::DasPluginFeature* p_out_feature) override {",
            "        (void)index;",
            "        if (p_out_feature == nullptr) {",
            "            return DAS_E_INVALID_POINTER;",
            "        }",
            "        return DAS_E_OUT_OF_RANGE;",
            "    }",
            "",
            "    DasResult CreateFeatureInterface(",
            "        uint64_t index,",
            "        IDasBase** pp_out_interface) override {",
            "        (void)index;",
            "        if (pp_out_interface == nullptr) {",
            "            return DAS_E_INVALID_POINTER;",
            "        }",
            "        *pp_out_interface = nullptr;",
            "        return DAS_E_OUT_OF_RANGE;",
            "    }",
            "",
            "    DasResult CanUnloadNow(bool* canUnloadNow) override {",
            "        if (canUnloadNow == nullptr) {",
            "            return DAS_E_INVALID_POINTER;",
            "        }",
            "        *canUnloadNow = true;",
            "        return DAS_S_OK;",
            "    }",
            "",
            "private:",
            "    std::atomic<uint32_t> ref_count_{1};",
            "    NodeHostBootstrapPaths paths_;",
            "};",
            "",
            "std::filesystem::path OptionalPathOption(",
            "    Napi::Env env,",
            "    Napi::Object options,",
            "    const char* name) {",
            "    Napi::Value value = options.Get(name);",
            "    if (value.IsUndefined() || value.IsNull()) {",
            "        return {};",
            "    }",
            "    if (!value.IsString()) {",
            "        throw Napi::TypeError::New(",
            "            env,",
            "            std::string{name} + \" must be a string\");",
            "    }",
            "    return std::filesystem::path(value.As<Napi::String>().Utf8Value());",
            "}",
            "",
            "NodeHostBootstrapPaths ReadNodeHostBootstrapPaths(",
            "    Napi::Env env,",
            "    Napi::Object options) {",
            "    NodeHostBootstrapPaths paths{};",
            "    paths.package_root = OptionalPathOption(env, options, \"packageRoot\");",
            "    paths.wrapper_path = OptionalPathOption(env, options, \"wrapperPath\");",
            "    paths.addon_path = OptionalPathOption(env, options, \"addonPath\");",
            "    return paths;",
            "}",
            "",
            "DAS::Utils::Expected<DAS::DasPtr<IDasBase>> LoadMinimalNodePackage(",
            "    const std::filesystem::path& manifest_path,",
            "    NodeHostBootstrapPaths paths) {",
            "    std::ifstream manifest_file(manifest_path);",
            "    if (!manifest_file.is_open()) {",
            "        return tl::make_unexpected(DAS_E_IPC_PLUGIN_LOAD_FAILED);",
            "    }",
            "",
            "    std::string manifest_content(",
            "        (std::istreambuf_iterator<char>(manifest_file)),",
            "        std::istreambuf_iterator<char>());",
            "    auto manifest_json_opt = Das::Utils::ParseYyjsonFromString(manifest_content);",
            "    if (!manifest_json_opt) {",
            "        return tl::make_unexpected(DAS_E_IPC_PLUGIN_LOAD_FAILED);",
            "    }",
            "    yyjson::value manifest_json = std::move(*manifest_json_opt);",
            "    auto manifest_obj = manifest_json.as_object();",
            "    if (!manifest_obj) {",
            "        return tl::make_unexpected(DAS_E_IPC_PLUGIN_LOAD_FAILED);",
            "    }",
            "",
            "    auto language_value = (*manifest_obj)[std::string_view(\"language\")];",
            "    auto language_str = language_value.as_string();",
            "    if (!language_str) {",
            "        return tl::make_unexpected(DAS_E_IPC_PLUGIN_LOAD_FAILED);",
            "    }",
            "    const std::string plugin_language(*language_str);",
            "    if (plugin_language != \"Node\") {",
            "        return tl::make_unexpected(DAS_E_IPC_PLUGIN_LOAD_FAILED);",
            "    }",
            "",
            "    if (paths.package_root.empty()) {",
            "        paths.package_root = manifest_path.parent_path();",
            "    }",
            "    return DAS::DasPtr<IDasBase>::Attach(",
            "        static_cast<IDasBase*>(new MinimalNodePluginPackage(std::move(paths))));",
            "}",
            "",
            "DasResult DAS_STD_CALL OnNodeHostBeforeShutdown(",
            "    void* context,",
            "    DAS::Core::IPC::Host::HostShutdownReason reason,",
            "    uint32_t timeout_ms) {",
            "    (void)context;",
            "    (void)reason;",
            "    (void)timeout_ms;",
            "    return DAS_S_OK;",
            "}",
            "",
            "Napi::Value startHostIpc(const Napi::CallbackInfo& info) {",
            "    Napi::Env env = info.Env();",
            "    if (info.Length() != 1 || !info[0].IsObject()) {",
            "        throw Napi::TypeError::New(env, \"startHostIpc(options) expects an object\");",
            "    }",
            "",
            "    Napi::Object options = info[0].As<Napi::Object>();",
            "    DAS::Core::IPC::Host::IpcContextConfig config{};",
            "    std::string connect_url_storage;",
            "    bool has_main_pid = false;",
            "    bool has_connect_url = false;",
            "",
            "    Napi::Value main_pid_value = options.Get(\"mainPid\");",
            "    if (!main_pid_value.IsUndefined() && !main_pid_value.IsNull()) {",
            "        if (!main_pid_value.IsNumber()) {",
            "            throw Napi::TypeError::New(env, \"mainPid must be a number\");",
            "        }",
            "        const double main_pid_number =",
            "            main_pid_value.As<Napi::Number>().DoubleValue();",
            "        if (main_pid_number <= 0.0",
            "            || main_pid_number > static_cast<double>(",
            "                std::numeric_limits<uint32_t>::max())) {",
            "            throw Napi::RangeError::New(env, \"mainPid is out of uint32 range\");",
            "        }",
            "        config.main_pid = static_cast<uint32_t>(main_pid_number);",
            "        has_main_pid = true;",
            "    }",
            "",
            "    Napi::Value connect_url_value = options.Get(\"connectUrl\");",
            "    if (!connect_url_value.IsUndefined() && !connect_url_value.IsNull()) {",
            "        if (!connect_url_value.IsString()) {",
            "            throw Napi::TypeError::New(env, \"connectUrl must be a string\");",
            "        }",
            "        connect_url_storage = connect_url_value.As<Napi::String>().Utf8Value();",
            "        if (connect_url_storage.empty()) {",
            "            throw Napi::TypeError::New(env, \"connectUrl must not be empty\");",
            "        }",
            "        config.connect_url = connect_url_storage.c_str();",
            "        has_connect_url = true;",
            "    }",
            "",
            "    if (!has_main_pid && !has_connect_url) {",
            "        throw Napi::TypeError::New(env, \"startHostIpc requires mainPid or connectUrl\");",
            "    }",
            "",
            "    NodeHostBootstrapPaths paths = ReadNodeHostBootstrapPaths(env, options);",
            "    config.events.on_before_shutdown = OnNodeHostBeforeShutdown;",
            "    DAS::Core::IPC::Host::IpcContextPtr ctx{",
            "        DAS::Core::IPC::Host::CreateIpcContext(config)};",
            "    if (!ctx) {",
            "        ThrowDasException(env, DAS_E_IPC_INVALID_STATE, \"CreateIpcContext failed\");",
            "        return env.Undefined();",
            "    }",
            "",
            "    DAS::Core::IPC::Host::HostCommandHandlerOptions handler_options{};",
            "    handler_options.load_plugin =",
            "        [paths = std::move(paths)](",
            "            const std::filesystem::path& manifest_path) mutable {",
            "            return LoadMinimalNodePackage(manifest_path, paths);",
            "        };",
            "    const DasResult register_result =",
            "        DAS::Core::IPC::Host::RegisterHostCommandHandlers(",
            "            ctx.get(),",
            "            std::move(handler_options));",
            "    if (register_result < 0) {",
            "        ThrowDasException(env, register_result, \"RegisterHostCommandHandlers failed\");",
            "        return env.Undefined();",
            "    }",
            "",
            "    const DasResult run_result = ctx->Run();",
            "    return Napi::Number::New(env, static_cast<double>(run_result));",
            "}",
            "",
        ]

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

    def _generate_cpp_director_base(self, doc: IdlDocument) -> list[str]:
        js_error = (
            "DAS_E_JAVASCRIPT_ERROR"
            if _doc_has_error_code(doc, "DAS_E_JAVASCRIPT_ERROR")
            else "DAS_E_FAIL"
        )
        missing_callback = (
            "DAS_E_JAVASCRIPT_NO_IMPLEMENTATION"
            if _doc_has_error_code(doc, "DAS_E_JAVASCRIPT_NO_IMPLEMENTATION")
            else "DAS_E_NO_IMPLEMENTATION"
        )
        return [
            f"constexpr DasResult kNapiDirectorJavaScriptError = {js_error};",
            f"constexpr DasResult kNapiDirectorMissingCallbackResult = {missing_callback};",
            "",
            "struct NapiDirectorCallData {",
            "    using Invoke = std::function<DasResult(Napi::Env)>;",
            "",
            "    explicit NapiDirectorCallData(Invoke in_invoke)",
            "        : invoke(std::move(in_invoke)) {}",
            "",
            "    Invoke invoke;",
            "    std::mutex mutex;",
            "    std::condition_variable complete;",
            "    bool done{false};",
            "    DasResult result{kNapiDirectorJavaScriptError};",
            "",
            "    void Run(Napi::Env env) {",
            "        Finish(invoke(env));",
            "    }",
            "",
            "    void Finish(DasResult in_result) {",
            "        {",
            "            std::lock_guard<std::mutex> lock(mutex);",
            "            result = in_result;",
            "            done = true;",
            "        }",
            "        complete.notify_one();",
            "    }",
            "",
            "    DasResult Wait() {",
            "        std::unique_lock<std::mutex> lock(mutex);",
            "        complete.wait(lock, [this] { return done; });",
            "        return result;",
            "    }",
            "};",
            "",
            "class NapiDirectorBase {",
            "protected:",
            "    using DirectorInvoke = std::function<DasResult(Napi::Env, Napi::Function)>;",
            "",
            "    NapiDirectorBase(Napi::Env env, Napi::Object callbacks)",
            "        : env_(env),",
            "          callbacks_(Napi::Persistent(callbacks)),",
            "          node_thread_id_(std::this_thread::get_id()) {",
            "        callbacks_.SuppressDestruct();",
            "        Napi::Function trampoline = Napi::Function::New(",
            "            env,",
            "            [](const Napi::CallbackInfo& finalizer_info) {",
            "                (void)finalizer_info;",
            "            });",
            "        tsfn_ = Napi::ThreadSafeFunction::New(",
            "            env,",
            "            trampoline,",
            '            "DAS NapiDirector upcall",',
            "            0,",
            "            1);",
            "    }",
            "",
            "    virtual ~NapiDirectorBase() {",
            "        // Finalizers and destructors release native references only; they never call JS callbacks.",
            "        callbacks_.Reset();",
            "        if (tsfn_) {",
            "            tsfn_.Release();",
            "        }",
            "    }",
            "",
            "    bool IsNodeThread() const {",
            "        return std::this_thread::get_id() == node_thread_id_;",
            "    }",
            "",
            "    Napi::Object CallbackThis() const {",
            "        return callbacks_.Value();",
            "    }",
            "",
            "    bool hasCallbackMethod(const char* method_name) const {",
            "        Napi::HandleScope scope(env_);",
            "        Napi::Value method = callbacks_.Value().Get(method_name);",
            "        return method.IsFunction();",
            "    }",
            "",
            "    DasResult DispatchDirectorCall(",
            "        const char* method_name,",
            "        DirectorInvoke invoke) {",
            "        UpcallGuard guard(this);",
            "        if (!guard.acquired()) {",
            '            LogDirectorError(method_name, "reentrant JavaScript director upcall rejected");',
            "            return kNapiDirectorJavaScriptError;",
            "        }",
            "        if (IsNodeThread()) {",
            "            return DispatchDirect(method_name, std::move(invoke));",
            "        }",
            "        return DispatchThreadSafe(method_name, std::move(invoke));",
            "    }",
            "",
            "    DasResult DispatchDirect(",
            "        const char* method_name,",
            "        DirectorInvoke invoke) {",
            "        Napi::HandleScope scope(env_);",
            "        if (!hasCallbackMethod(method_name)) {",
            "            LogMissingCallback(method_name);",
            "            return kNapiDirectorMissingCallbackResult;",
            "        }",
            "        try {",
            "            Napi::Value method = callbacks_.Value().Get(method_name);",
            "            Napi::Function callback = method.As<Napi::Function>();",
            "            return invoke(env_, callback);",
            "        } catch (const Napi::Error& error) {",
            "            LogJavaScriptException(method_name, error);",
            "            return kNapiDirectorJavaScriptError;",
            "        } catch (const std::exception& error) {",
            "            LogDirectorError(method_name, error.what());",
            "            return kNapiDirectorJavaScriptError;",
            "        } catch (...) {",
            '            LogDirectorError(method_name, "unknown native exception during JavaScript director upcall");',
            "            return kNapiDirectorJavaScriptError;",
            "        }",
            "    }",
            "",
            "    DasResult DispatchThreadSafe(",
            "        const char* method_name,",
            "        DirectorInvoke invoke) {",
            "        auto call = std::make_shared<NapiDirectorCallData>(",
            "            [this, method_name, invoke = std::move(invoke)](Napi::Env env) mutable {",
            "                (void)env;",
            "                return DispatchDirect(method_name, std::move(invoke));",
            "            });",
            "        napi_status status = tsfn_.BlockingCall(",
            "            call.get(),",
            "            [](Napi::Env env, Napi::Function, NapiDirectorCallData* call) {",
            "                call->Run(env);",
            "            });",
            "        if (status != napi_ok) {",
            '            LogDirectorError(method_name, "failed to marshal JavaScript director upcall to Node thread");',
            "            return kNapiDirectorJavaScriptError;",
            "        }",
            "        return call->Wait();",
            "    }",
            "",
            "    void LogMissingCallback(const char* method_name) const {",
            "        std::fprintf(",
            "            stderr,",
            '            "[DAS NapiDirector] missing JavaScript callback: %s\\n",',
            "            method_name);",
            "    }",
            "",
            "    void LogDirectorError(const char* method_name, const char* message) const {",
            "        std::fprintf(",
            "            stderr,",
            '            "[DAS NapiDirector] %s failed: %s\\n",',
            "            method_name,",
            "            message ? message : \"unknown error\");",
            "    }",
            "",
            "    void LogJavaScriptException(const char* method_name, const Napi::Error& error) const {",
            "        std::fprintf(",
            "            stderr,",
            '            "[DAS NapiDirector] %s threw: %s\\n",',
            "            method_name,",
            "            error.Message().c_str());",
            '        Napi::Value stack = error.Value().As<Napi::Object>().Get("stack");',
            "        if (stack.IsString()) {",
            "            const std::string stack_text = stack.As<Napi::String>().Utf8Value();",
            "            std::fprintf(stderr, \"[DAS NapiDirector] stack: %s\\n\", stack_text.c_str());",
            "        }",
            "    }",
            "",
            "    class UpcallGuard {",
            "    public:",
            "        explicit UpcallGuard(NapiDirectorBase* director)",
            "            : director_(director) {",
            "            bool expected = false;",
            "            acquired_ = director_->upcall_active_.compare_exchange_strong(",
            "                expected,",
            "                true,",
            "                std::memory_order_acq_rel);",
            "        }",
            "",
            "        ~UpcallGuard() {",
            "            if (acquired_) {",
            "                director_->upcall_active_.store(false, std::memory_order_release);",
            "            }",
            "        }",
            "",
            "        bool acquired() const { return acquired_; }",
            "",
            "    private:",
            "        NapiDirectorBase* director_;",
            "        bool acquired_{false};",
            "    };",
            "",
            "private:",
            "    Napi::Env env_;",
            "    Napi::ObjectReference callbacks_;",
            "    std::thread::id node_thread_id_;",
            "    Napi::ThreadSafeFunction tsfn_;",
            "    std::atomic_bool upcall_active_{false};",
            "};",
        ]

    def _generate_cpp_director_class(
        self,
        interface: InterfaceDef,
        doc: IdlDocument,
    ) -> list[str]:
        director_name = self._director_name(interface.name)
        lines = [
            f"class {director_name} final",
            f"    : public {interface.name}, public NapiDirectorBase {{",
            "public:",
            f"    {director_name}(Napi::Env env, Napi::Object callbacks)",
            "        : NapiDirectorBase(env, callbacks) {}",
            "",
            "    std::atomic<uint32_t> ref_count_{1};",
            "",
            "    uint32_t AddRef() override {",
            "        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;",
            "    }",
            "",
            "    uint32_t Release() override {",
            "        const uint32_t count = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;",
            "        if (count == 0) {",
            "            delete this;",
            "        }",
            "        return count;",
            "    }",
            "",
            "    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override {",
            "        if (pp_object == nullptr) {",
            "            return DAS_E_INVALID_POINTER;",
            "        }",
        ]
        for interface_name in _interface_chain(interface, doc):
            lines.extend(
                [
                    f"        if (iid == DasIidOf<{interface_name}>()) {{",
                    f"            *pp_object = static_cast<{interface_name}*>(this);",
                    "            AddRef();",
                    "            return DAS_S_OK;",
                    "        }",
                ]
            )
        lines.extend(
            [
                "        if (iid == DAS_IID_BASE) {",
                "            *pp_object = static_cast<IDasBase*>(this);",
                "            AddRef();",
                "            return DAS_S_OK;",
                "        }",
                "        *pp_object = nullptr;",
                "        return DAS_E_NO_INTERFACE;",
                "    }",
            ]
        )
        for method in _interface_methods_including_bases(interface, doc):
            lines.append("")
            lines.extend(self._generate_cpp_director_method(method, doc))
        lines.append("};")
        return lines

    def _generate_cpp_director_create_function(
        self,
        interface: InterfaceDef,
    ) -> list[str]:
        director_name = self._director_name(interface.name)
        wrapper_name = _cpp_wrapper_name(interface.name)
        return [
            f"Napi::Value create{director_name}(const Napi::CallbackInfo& info) {{",
            "    Napi::Env env = info.Env();",
            "    if (info.Length() != 1 || !info[0].IsObject()) {",
            f'        throw Napi::TypeError::New(env, "{director_name} expects a callback object");',
            "    }",
            f"    auto* director = new {director_name}(env, info[0].As<Napi::Object>());",
            f"    return {wrapper_name}::WrapAdopted(env, director);",
            "}",
        ]

    def _generate_cpp_director_method(
        self,
        method: MethodDef,
        doc: IdlDocument,
    ) -> list[str]:
        method_label = public_js_name(method.name)
        param_decls = [
            f"{_cpp_type(param.type_info)} {_sanitize_identifier(param.name)}"
            for param in method.parameters
        ]
        lines = [
            f"    DasResult {method.name}({', '.join(param_decls)}) override {{",
        ]
        out_params = _method_out_params(method)
        for param in out_params:
            name = _sanitize_identifier(param.name)
            lines.extend(
                [
                    f"        if ({name} == nullptr) {{",
                    "            return DAS_E_INVALID_POINTER;",
                    "        }",
                ]
            )
            if _value_type_for_out_param(param).type_kind == TypeKind.INTERFACE:
                lines.append(f"        *{name} = nullptr;")

        capture_names: list[str] = ["this"]
        for param in _method_in_params(method):
            capture_lines, capture_name = self._generate_cpp_director_capture(
                param,
                doc,
            )
            lines.extend(capture_lines)
            capture_names.append(capture_name)
        for param in out_params:
            capture_names.append(_sanitize_identifier(param.name))

        capture_list = ", ".join(capture_names)
        lines.extend(
            [
                f'        return DispatchDirectorCall("{method_label}",',
                f"            [{capture_list}](Napi::Env env, Napi::Function callback) mutable -> DasResult {{",
                "                std::vector<napi_value> js_args;",
            ]
        )
        for param in _method_in_params(method):
            lines.extend(self._generate_cpp_director_js_arg(param, doc))
        lines.extend(
            [
                "                Napi::Value js_result = callback.Call(CallbackThis(), js_args);",
            ]
        )
        lines.extend(
            self._generate_cpp_director_result_conversion(
                method,
                doc,
                method_label,
            )
        )
        lines.extend(
            [
                "            });",
                "    }",
            ]
        )
        return lines

    def _generate_cpp_director_capture(
        self,
        param: ParameterDef,
        doc: IdlDocument,
    ) -> tuple[list[str], str]:
        name = _sanitize_identifier(param.name)
        capture_name = f"{name}_copy"
        mapped = _map_type(param.type_info)
        if mapped is not None:
            if mapped.category == "utf8_string":
                return [
                    f"        if ({name} == nullptr) {{",
                    "            return DAS_E_INVALID_POINTER;",
                    "        }",
                    f"        std::string {capture_name}{{{name}}};",
                ], capture_name
            if mapped.category == "readonly_string":
                utf8_name = f"{name}_utf8"
                result_name = f"{name}_utf8_result"
                return [
                    f"        if ({name} == nullptr) {{",
                    "            return DAS_E_INVALID_POINTER;",
                    "        }",
                    f"        const char* {utf8_name} = nullptr;",
                    f"        const DasResult {result_name} = {name}->GetUtf8(&{utf8_name});",
                    f"        if ({result_name} < 0) {{",
                    f"            return {result_name};",
                    "        }",
                    f"        if ({utf8_name} == nullptr) {{",
                    "            return DAS_E_INVALID_POINTER;",
                    "        }",
                    f"        std::string {capture_name}{{{utf8_name}}};",
                ], capture_name
            return [
                f"        const {_cpp_storage_type(param.type_info)} {capture_name} = {name};",
            ], capture_name

        if param.type_info.type_kind == TypeKind.STRUCT:
            if param.type_info.is_pointer:
                return [
                    f"        if ({name} == nullptr) {{",
                    "            return DAS_E_INVALID_POINTER;",
                    "        }",
                    f"        const {type_simple_name(param.type_info)} {capture_name} = *{name};",
                ], capture_name
            return [
                f"        const {type_simple_name(param.type_info)} {capture_name} = {name};",
            ], capture_name

        if param.type_info.type_kind == TypeKind.INTERFACE:
            interface_name = type_simple_name(param.type_info)
            capture_name = f"{name}_hold"
            return [
                f"        if ({name} == nullptr) {{",
                "            return DAS_E_INVALID_POINTER;",
                "        }",
                f"        DAS::DasPtr<{interface_name}> {capture_name}({name});",
            ], capture_name

        raise AssertionError(f"unsupported director input parameter {param.name}")

    def _generate_cpp_director_js_arg(
        self,
        param: ParameterDef,
        doc: IdlDocument,
    ) -> list[str]:
        name = _sanitize_identifier(param.name)
        capture_name = f"{name}_copy"
        arg_name = f"{name}_arg"
        mapped = _map_type(param.type_info)
        if mapped is not None:
            if mapped.category == "das_guid":
                return [
                    f"                Napi::Value {arg_name} = WriteDasGuid(env, {capture_name});",
                    f"                js_args.push_back({arg_name});",
                ]
            if mapped.category in {"int32", "uint32", "number", "enum", "das_result"}:
                return [
                    f"                Napi::Value {arg_name} = Napi::Number::New(env, static_cast<double>({capture_name}));",
                    f"                js_args.push_back({arg_name});",
                ]
            if mapped.category == "boolean":
                return [
                    f"                Napi::Value {arg_name} = Napi::Boolean::New(env, {capture_name} != 0);",
                    f"                js_args.push_back({arg_name});",
                ]
            if mapped.category == "int64":
                return [
                    f"                Napi::Value {arg_name} = Napi::BigInt::New(env, static_cast<int64_t>({capture_name}));",
                    f"                js_args.push_back({arg_name});",
                ]
            if mapped.category == "uint64":
                return [
                    f"                Napi::Value {arg_name} = Napi::BigInt::New(env, static_cast<uint64_t>({capture_name}));",
                    f"                js_args.push_back({arg_name});",
                ]
            if mapped.category in {"utf8_string", "readonly_string"}:
                return [
                    f"                Napi::Value {arg_name} = Napi::String::New(env, {capture_name});",
                    f"                js_args.push_back({arg_name});",
                ]
            raise AssertionError(f"unsupported director mapped input {param.name}")

        if param.type_info.type_kind == TypeKind.STRUCT:
            struct = _find_struct(doc, param.type_info)
            if struct is None:
                raise AssertionError(f"missing struct {type_simple_name(param.type_info)}")
            lines = self._generate_cpp_struct_to_js_object(
                doc,
                struct,
                capture_name,
                arg_name,
                indent="                ",
            )
            lines.append(f"                js_args.push_back({arg_name});")
            return lines

        if param.type_info.type_kind == TypeKind.INTERFACE:
            interface_name = type_simple_name(param.type_info)
            capture_name = f"{name}_hold"
            if _is_binary_buffer_interface(param.type_info):
                return [
                    f"                Napi::Value {arg_name} = ConvertIDasBinaryBufferToBuffer(",
                    "                    env,",
                    f"                    {capture_name}.Get(),",
                    "                    BinaryBufferOwnershipMode::BorrowAddRef);",
                    f"                js_args.push_back({arg_name});",
                ]
            return [
                f"                Napi::Value {arg_name} = {_cpp_wrapper_name(interface_name)}::WrapBorrowed(env, {capture_name}.Get());",
                f"                js_args.push_back({arg_name});",
            ]

        raise AssertionError(f"unsupported director JS argument {param.name}")

    def _generate_cpp_struct_to_js_object(
        self,
        doc: IdlDocument,
        struct: StructDef,
        value_expr: str,
        object_name: str,
        *,
        indent: str,
    ) -> list[str]:
        lines = [f"{indent}Napi::Object {object_name} = Napi::Object::New(env);"]
        for field in struct.fields:
            field_type = TypeInfo(field.type_name)
            field_value_expr = f"{value_expr}.{field.name}"
            field_js_name = public_js_name(field.name)
            mapped = _map_type(field_type)
            if mapped is not None:
                lines.append(
                    f'{indent}{object_name}.Set("{field_js_name}", '
                    f"{self._cpp_return_expression(field_value_expr, mapped)});"
                )
                continue

            nested_struct = _find_struct(doc, field_type)
            if nested_struct is None:
                raise AssertionError(f"unsupported struct field type {field.type_name}")
            nested_object_name = f"{object_name}_{_sanitize_identifier(field.name)}"
            lines.extend(
                self._generate_cpp_struct_to_js_object(
                    doc,
                    nested_struct,
                    field_value_expr,
                    nested_object_name,
                    indent=indent,
                )
            )
            lines.append(f'{indent}{object_name}.Set("{field_js_name}", {nested_object_name});')
        return lines

    def _generate_cpp_director_result_conversion(
        self,
        method: MethodDef,
        doc: IdlDocument,
        method_label: str,
    ) -> list[str]:
        out_params = _method_out_params(method)
        if not out_params:
            return [
                "                if (!js_result.IsNumber()) {",
                f'                    LogDirectorError("{method_label}", "callback must return DasResult");',
                "                    return kNapiDirectorJavaScriptError;",
                "                }",
                "                return static_cast<DasResult>(js_result.As<Napi::Number>().Int32Value());",
            ]
        if len(out_params) == 1:
            return self._generate_cpp_director_assign_out_param(
                out_params[0],
                "js_result",
                method_label,
                doc,
                indent="                ",
            ) + ["                return DAS_S_OK;"]

        lines = [
            "                if (!js_result.IsObject()) {",
            f'                    LogDirectorError("{method_label}", "callback must return an object");',
            "                    return kNapiDirectorJavaScriptError;",
            "                }",
            "                Napi::Object js_output = js_result.As<Napi::Object>();",
        ]
        for param in out_params:
            field_name = clean_out_field_name(param.name)
            value_name = f"{_sanitize_identifier(param.name)}_js_value"
            lines.append(
                f'                Napi::Value {value_name} = js_output.Get("{field_name}");'
            )
            lines.extend(
                self._generate_cpp_director_assign_out_param(
                    param,
                    value_name,
                    method_label,
                    doc,
                    indent="                ",
                )
            )
        lines.append("                return DAS_S_OK;")
        return lines

    def _generate_cpp_director_assign_out_param(
        self,
        param: ParameterDef,
        value_expr: str,
        method_label: str,
        doc: IdlDocument,
        *,
        indent: str,
    ) -> list[str]:
        value_type = _value_type_for_out_param(param)
        target = f"*{_sanitize_identifier(param.name)}"
        label = clean_out_field_name(param.name)

        if _is_binary_buffer_interface(value_type):
            native_name = f"{_sanitize_identifier(param.name)}_native"
            return [
                f"{indent}if (!{value_expr}.IsBuffer()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be Buffer");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}auto* {native_name} = new NapiDirectorBinaryBuffer(",
                f"{indent}    {value_expr}.As<Napi::Buffer<unsigned char>>());",
                f"{indent}{target} = {native_name};",
            ]

        if _is_binary_buffer_like(value_type):
            return [
                f'{indent}LogDirectorError("{method_label}", "raw binary buffer callback returns are not supported");',
                f"{indent}return kNapiDirectorJavaScriptError;",
            ]

        if _is_readonly_string_interface_pointer(value_type):
            created_name = f"{_sanitize_identifier(param.name)}_created"
            storage_name = f"{_sanitize_identifier(param.name)}_string"
            return [
                f"{indent}if (!{value_expr}.IsString()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be string");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}const std::string {storage_name} = {value_expr}.As<Napi::String>().Utf8Value();",
                f"{indent}IDasReadOnlyString* {created_name} = nullptr;",
                f"{indent}const DasResult {created_name}_result = CreateIDasReadOnlyStringFromUtf8(",
                f"{indent}    {storage_name}.c_str(), &{created_name});",
                f"{indent}if ({created_name}_result < 0 || {created_name} == nullptr) {{",
                f'{indent}    LogDirectorError("{method_label}", "failed to create IDasReadOnlyString callback return");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}{target} = {created_name};",
            ]

        if value_type.type_kind == TypeKind.INTERFACE:
            interface_name = type_simple_name(value_type)
            native_name = f"{_sanitize_identifier(param.name)}_native"
            return [
                f"{indent}if (!{value_expr}.IsObject()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be an interface wrapper");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}auto* {native_name} = {_cpp_wrapper_name(interface_name)}::UnwrapHandle(env, {value_expr});",
                f"{indent}{native_name}->AddRef();",
                f"{indent}{target} = {native_name};",
            ]

        if value_type.type_kind == TypeKind.STRUCT:
            struct = _find_struct(doc, value_type)
            if struct is None:
                raise AssertionError(f"missing struct {type_simple_name(value_type)}")
            object_name = f"{_sanitize_identifier(param.name)}_object"
            struct_target = f"(*{_sanitize_identifier(param.name)})"
            lines = [
                f"{indent}if (!{value_expr}.IsObject()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be object");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}Napi::Object {object_name} = {value_expr}.As<Napi::Object>();",
            ]
            for field in struct.fields:
                field_value = f'{object_name}.Get("{public_js_name(field.name)}")'
                lines.extend(
                    self._generate_cpp_director_assign_value(
                        TypeInfo(field.type_name),
                        field_value,
                        f"{struct_target}.{field.name}",
                        f"{label}.{public_js_name(field.name)}",
                        method_label,
                        doc,
                        indent=indent,
                    )
                )
            return lines

        return self._generate_cpp_director_assign_value(
            value_type,
            value_expr,
            target,
            label,
            method_label,
            doc,
            indent=indent,
        )

    def _generate_cpp_director_assign_value(
        self,
        value_type: TypeInfo,
        value_expr: str,
        target_expr: str,
        label: str,
        method_label: str,
        doc: IdlDocument,
        *,
        indent: str,
    ) -> list[str]:
        mapped = _map_type(value_type)
        if mapped is None:
            nested_struct = _find_struct(doc, value_type)
            if nested_struct is None:
                raise AssertionError(f"unsupported director output type {type_simple_name(value_type)}")
            object_name = f"{_sanitize_identifier(label)}_object"
            lines = [
                f"{indent}if (!{value_expr}.IsObject()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be object");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}Napi::Object {object_name} = {value_expr}.As<Napi::Object>();",
            ]
            for field in nested_struct.fields:
                field_label = f"{label}.{public_js_name(field.name)}"
                field_value = f'{object_name}.Get("{public_js_name(field.name)}")'
                lines.extend(
                    self._generate_cpp_director_assign_value(
                        TypeInfo(field.type_name),
                        field_value,
                        f"{target_expr}.{field.name}",
                        field_label,
                        method_label,
                        doc,
                        indent=indent,
                    )
                )
            return lines

        if mapped.category == "das_guid":
            return [
                f"{indent}if (!{value_expr}.IsString()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be string");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}{target_expr} = ReadDasGuid(env, {value_expr});",
            ]
        if mapped.category in {"int32", "enum", "das_result"}:
            return [
                f"{indent}if (!{value_expr}.IsNumber()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be number");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}{target_expr} = static_cast<{_cpp_storage_type(value_type)}>({value_expr}.As<Napi::Number>().Int32Value());",
            ]
        if mapped.category == "uint32":
            return [
                f"{indent}if (!{value_expr}.IsNumber()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be number");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}{target_expr} = static_cast<{_cpp_storage_type(value_type)}>({value_expr}.As<Napi::Number>().Uint32Value());",
            ]
        if mapped.category == "number":
            return [
                f"{indent}if (!{value_expr}.IsNumber()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be number");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}{target_expr} = static_cast<{_cpp_storage_type(value_type)}>({value_expr}.As<Napi::Number>().DoubleValue());",
            ]
        if mapped.category == "boolean":
            bool_expr = f"{value_expr}.As<Napi::Boolean>().Value()"
            if type_simple_name(value_type) == "DasBool":
                bool_expr = f"{bool_expr} ? DAS_TRUE : DAS_FALSE"
            return [
                f"{indent}if (!{value_expr}.IsBoolean()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be boolean");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}{target_expr} = {bool_expr};",
            ]
        if mapped.category == "int64":
            lossless = f"{_sanitize_identifier(label)}_lossless"
            raw = f"{_sanitize_identifier(label)}_raw"
            return [
                f"{indent}if (!{value_expr}.IsBigInt()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be bigint");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}bool {lossless} = false;",
                f"{indent}const int64_t {raw} = {value_expr}.As<Napi::BigInt>().Int64Value(&{lossless});",
                f"{indent}if (!{lossless}) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} bigint is out of int64 range");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}{target_expr} = static_cast<{_cpp_storage_type(value_type)}>({raw});",
            ]
        if mapped.category == "uint64":
            lossless = f"{_sanitize_identifier(label)}_lossless"
            raw = f"{_sanitize_identifier(label)}_raw"
            return [
                f"{indent}if (!{value_expr}.IsBigInt()) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} must be bigint");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}bool {lossless} = false;",
                f"{indent}const uint64_t {raw} = {value_expr}.As<Napi::BigInt>().Uint64Value(&{lossless});",
                f"{indent}if (!{lossless}) {{",
                f'{indent}    LogDirectorError("{method_label}", "{label} bigint is out of uint64 range");',
                f"{indent}    return kNapiDirectorJavaScriptError;",
                f"{indent}}}",
                f"{indent}{target_expr} = static_cast<{_cpp_storage_type(value_type)}>({raw});",
            ]
        raise AssertionError(f"unsupported director output category {mapped.category}")

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
        for method in _interface_declared_methods(interface):
            js_name = public_js_name(method.name)
            wrapper_name = f"{_sanitize_identifier(interface.name)}_{_sanitize_identifier(js_name)}"
            unsupported_reason = self._unsupported_interface_method_reason(method, doc)
            if unsupported_reason:
                lines.extend(
                    [
                        f"Napi::Value {wrapper_name}(const Napi::CallbackInfo& info) {{",
                        f'    return MakeUnsupported(info, "{interface.name}.{js_name}", "{_escape_cpp_string(unsupported_reason)}");',
                    ]
                )
            else:
                lines.extend(
                    self._generate_supported_cpp_interface_method(
                        interface,
                        method,
                        wrapper_name,
                        doc,
                    )
                )
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

    def _unsupported_interface_method_reason(
        self,
        method: MethodDef,
        doc: IdlDocument,
    ) -> str:
        mapped_return = _map_type(method.return_type, for_return=True)
        if mapped_return is None or mapped_return.category != "das_result":
            return "interface wrapper methods currently require DasResult returns"

        for param in method.parameters:
            if param.direction == ParamDirection.INOUT:
                return "inout parameters are not supported by NAPI wrapper methods"
            if param.direction == ParamDirection.IN:
                reason = self._unsupported_in_param_reason(param, doc)
            elif param.direction == ParamDirection.OUT:
                reason = self._unsupported_out_param_reason(param, doc)
            else:
                reason = f"unsupported parameter direction {param.direction.value}"
            if reason:
                return reason
        return ""

    def _unsupported_in_param_reason(self, param: ParameterDef, doc: IdlDocument) -> str:
        if _map_type(param.type_info) is not None:
            return ""
        if param.type_info.type_kind == TypeKind.STRUCT:
            if _find_struct(doc, param.type_info) is None:
                return f"struct type {type_simple_name(param.type_info)} is not available"
            return self._unsupported_struct_reason(param.type_info, doc)
        if (
            param.type_info.type_kind == TypeKind.INTERFACE
            and param.type_info.is_pointer
            and param.type_info.pointer_level == 1
        ):
            return ""
        return _unsupported_reason_for_type(param.type_info)

    def _unsupported_out_param_reason(self, param: ParameterDef, doc: IdlDocument) -> str:
        if not param.type_info.is_pointer:
            return "out parameters must be pointers"
        value_type = _value_type_for_out_param(param)
        if _is_binary_buffer_interface(value_type):
            return ""
        if _map_type(value_type) is not None:
            return ""
        if value_type.type_kind == TypeKind.STRUCT:
            if _find_struct(doc, value_type) is None:
                return f"struct type {type_simple_name(value_type)} is not available"
            return self._unsupported_struct_reason(value_type, doc)
        if value_type.type_kind == TypeKind.INTERFACE:
            return ""
        return _unsupported_reason_for_type(value_type, for_return=True)

    def _unsupported_struct_reason(self, type_info: TypeInfo, doc: IdlDocument) -> str:
        struct = _find_struct(doc, type_info)
        if struct is None:
            return f"struct type {type_simple_name(type_info)} is not available"
        for field in struct.fields:
            field_type = TypeInfo(field.type_name)
            if _map_type(field_type) is None:
                nested_reason = self._unsupported_struct_reason(field_type, doc)
                if nested_reason:
                    return nested_reason
        return ""

    def _generate_supported_cpp_interface_method(
        self,
        interface: InterfaceDef,
        method: MethodDef,
        wrapper_name: str,
        doc: IdlDocument,
    ) -> list[str]:
        in_params = _method_in_params(method)
        out_params = _method_out_params(method)
        lines = [
            f"Napi::Value {wrapper_name}(const Napi::CallbackInfo& info) {{",
            "    Napi::Env env = info.Env();",
            f"    if (info.Length() != {1 + len(in_params)}) {{",
            "        throw Napi::TypeError::New(",
            "            env,",
            f'            "{interface.name}.{public_js_name(method.name)} expects {len(in_params)} argument(s)");',
            "    }",
            f"    auto* native = {_cpp_wrapper_name(interface.name)}::UnwrapHandle(env, info[0]);",
        ]

        call_args_by_param: dict[str, str] = {}
        cleanup_lines: list[str] = []
        for offset, param in enumerate(in_params, start=1):
            conversion_lines, call_arg = self._generate_cpp_method_in_param(
                offset,
                param,
                doc,
                cleanup_lines,
            )
            lines.extend(conversion_lines)
            call_args_by_param[param.name] = call_arg

        for param in out_params:
            declaration_lines, call_arg = self._generate_cpp_out_declaration(param)
            lines.extend(declaration_lines)
            call_args_by_param[param.name] = call_arg

        call_args = [call_args_by_param[param.name] for param in method.parameters]
        lines.append(
            f"    const DasResult result = native->{method.name}({', '.join(call_args)});"
        )
        lines.extend(cleanup_lines)
        lines.extend(
            [
                "    if (result < 0) {",
                f'        ThrowDasException(env, result, "{interface.name}.{public_js_name(method.name)} failed");',
                "        return env.Undefined();",
                "    }",
            ]
        )
        lines.extend(self._generate_cpp_method_success_return(method, doc))
        return lines

    def _generate_cpp_method_in_param(
        self,
        index: int,
        param: ParameterDef,
        doc: IdlDocument,
        cleanup_lines: list[str],
    ) -> tuple[list[str], str]:
        mapped = _map_type(param.type_info)
        if mapped is not None:
            return (
                self._generate_param_conversion(index, param, mapped, cleanup_lines),
                self._call_argument(param, mapped),
            )

        name = _sanitize_identifier(param.name)
        if param.type_info.type_kind == TypeKind.STRUCT:
            struct = _find_struct(doc, param.type_info)
            if struct is None:
                raise AssertionError(f"missing struct {type_simple_name(param.type_info)}")
            lines = self._generate_cpp_struct_conversion(index, param, struct, doc)
            if param.type_info.is_pointer:
                return lines, f"&{name}_value"
            return lines, f"{name}_value"

        if param.type_info.type_kind == TypeKind.INTERFACE:
            interface_name = type_simple_name(param.type_info)
            lines = [
                f"    {interface_name}* {name}_value = {_cpp_wrapper_name(interface_name)}::UnwrapHandle(env, info[{index}]);",
            ]
            return lines, f"{name}_value"

        raise AssertionError(f"unsupported method input parameter {param.name}")

    def _generate_cpp_struct_conversion(
        self,
        index: int,
        param: ParameterDef,
        struct: StructDef,
        doc: IdlDocument,
    ) -> list[str]:
        name = _sanitize_identifier(param.name)
        object_name = f"{name}_object"
        value_name = f"{name}_value"
        lines = [
            f"    if (!info[{index}].IsObject()) {{",
            "        throw Napi::TypeError::New(",
            "            env,",
            f'            "{param.name} must be object");',
            "    }",
            f"    Napi::Object {object_name} = info[{index}].As<Napi::Object>();",
            f"    {type_simple_name(param.type_info)} {value_name}{{}};",
        ]
        for field in struct.fields:
            lines.extend(
                self._generate_cpp_struct_field_conversion(
                    param,
                    object_name,
                    value_name,
                    _sanitize_identifier(value_name),
                    field.name,
                    field.type_name,
                    doc,
                )
            )
        return lines

    def _generate_cpp_struct_field_conversion(
        self,
        param: ParameterDef,
        object_name: str,
        target_expr: str,
        variable_prefix: str,
        field_name: str,
        field_type_name: str,
        doc: IdlDocument,
    ) -> list[str]:
        field_js_name = public_js_name(field_name)
        field_expr = f'{object_name}.Get("{field_js_name}")'
        field_label = f"{param.name}.{field_js_name}"
        mapped = _map_type(TypeInfo(field_type_name))
        if mapped is None:
            nested_struct = _find_struct(doc, TypeInfo(field_type_name))
            if nested_struct is None:
                raise AssertionError(f"unsupported struct field type {field_type_name}")
            nested_object_name = f"{variable_prefix}_{_sanitize_identifier(field_name)}_object"
            nested_target = f"{target_expr}.{field_name}"
            nested_prefix = f"{variable_prefix}_{_sanitize_identifier(field_name)}"
            lines = [
                f"    if (!{field_expr}.IsObject()) {{",
                "        throw Napi::TypeError::New(",
                "            env,",
                f'            "{field_label} must be object");',
                "    }",
                f"    Napi::Object {nested_object_name} = {field_expr}.As<Napi::Object>();",
            ]
            for nested_field in nested_struct.fields:
                lines.extend(
                    self._generate_cpp_struct_field_conversion(
                        param,
                        nested_object_name,
                        nested_target,
                        nested_prefix,
                        nested_field.name,
                        nested_field.type_name,
                        doc,
                    )
                )
            return lines

        field_target = f"{target_expr}.{field_name}"
        field_variable_prefix = f"{variable_prefix}_{_sanitize_identifier(field_name)}"

        lines = [
            f"    if (!{field_expr}.Is{self._napi_type_check(mapped)}()) {{",
            "        throw Napi::TypeError::New(",
            "            env,",
            f'            "{field_label} must be {mapped.js_check}");',
            "    }",
        ]
        if mapped.category == "int64":
            lossless_name = f"{field_variable_prefix}_lossless"
            raw_name = f"{field_variable_prefix}_raw"
            lines.append(f"    bool {lossless_name} = false;")
            lines.append(
                f"    const int64_t {raw_name} = {field_expr}.As<Napi::BigInt>().Int64Value(&{lossless_name});"
            )
            lines.append(f"    if (!{lossless_name}) {{")
            lines.append(
                "        throw Napi::RangeError::New(env, \"bigint value is out of int64 range\");"
            )
            lines.append("    }")
            lines.append(
                f"    {field_target} = static_cast<{field_type_name}>({raw_name});"
            )
        elif mapped.category == "uint64":
            lossless_name = f"{field_variable_prefix}_lossless"
            raw_name = f"{field_variable_prefix}_raw"
            lines.append(f"    bool {lossless_name} = false;")
            lines.append(
                f"    const uint64_t {raw_name} = {field_expr}.As<Napi::BigInt>().Uint64Value(&{lossless_name});"
            )
            lines.append(f"    if (!{lossless_name}) {{")
            lines.append(
                "        throw Napi::RangeError::New(env, \"bigint value is out of uint64 range\");"
            )
            lines.append("    }")
            lines.append(
                f"    {field_target} = static_cast<{field_type_name}>({raw_name});"
            )
        elif mapped.category in {"int32", "enum", "das_result"}:
            lines.append(
                f"    {field_target} = static_cast<{field_type_name}>("
            )
            lines.append(f"        {field_expr}.As<Napi::Number>().Int32Value());")
        elif mapped.category == "uint32":
            lines.append(
                f"    {field_target} = static_cast<{field_type_name}>("
            )
            lines.append(f"        {field_expr}.As<Napi::Number>().Uint32Value());")
        elif mapped.category == "number":
            lines.append(
                f"    {field_target} = static_cast<{field_type_name}>("
            )
            lines.append(f"        {field_expr}.As<Napi::Number>().DoubleValue());")
        elif mapped.category == "boolean":
            lines.append(
                f"    {field_target} = {field_expr}.As<Napi::Boolean>().Value();"
            )
        else:
            raise AssertionError(f"unsupported struct field category {mapped.category}")
        return lines

    def _generate_cpp_out_declaration(
        self,
        param: ParameterDef,
    ) -> tuple[list[str], str]:
        value_type = _value_type_for_out_param(param)
        name = f"{_sanitize_identifier(param.name)}_value"
        if value_type.type_kind == TypeKind.INTERFACE:
            return [f"    {_cpp_type(value_type)} {name} = nullptr;"], f"&{name}"
        return [f"    {_cpp_storage_type(value_type)} {name}{{}};"], f"&{name}"

    def _generate_cpp_method_success_return(
        self,
        method: MethodDef,
        doc: IdlDocument,
    ) -> list[str]:
        out_params = _method_out_params(method)
        if not out_params:
            return [
                "    return Napi::Number::New(env, static_cast<double>(result));",
            ]
        if len(out_params) == 1:
            value_lines, value_expr = self._generate_cpp_out_value_expression(
                out_params[0],
                method,
                doc,
            )
            return [*value_lines, f"    return {value_expr};"]

        lines = ["    Napi::Object output = Napi::Object::New(env);"]
        for param in out_params:
            field_name = clean_out_field_name(param.name)
            value_lines, value_expr = self._generate_cpp_out_value_expression(
                param,
                method,
                doc,
            )
            lines.extend(value_lines)
            lines.append(f'    output.Set("{field_name}", {value_expr});')
        lines.append("    return output;")
        return lines

    def _generate_cpp_out_value_expression(
        self,
        param: ParameterDef,
        method: MethodDef,
        doc: IdlDocument,
    ) -> tuple[list[str], str]:
        value_type = _value_type_for_out_param(param)
        value_name = f"{_sanitize_identifier(param.name)}_value"
        if _is_binary_buffer_interface(value_type):
            return self._generate_cpp_binary_buffer_out_value(param)
        if _is_readonly_string_interface_pointer(value_type):
            return self._generate_cpp_owned_interface_out_value(
                param,
                value_type,
                f"ConvertDasReadOnlyStringToString(env, {_sanitize_identifier(param.name)}_owned.Get())",
            )
        if value_type.type_kind == TypeKind.INTERFACE:
            wrapper_name = _cpp_wrapper_name(type_simple_name(value_type))
            return self._generate_cpp_owned_interface_out_value(
                param,
                value_type,
                f"{wrapper_name}::WrapAdopted(env, std::move({_sanitize_identifier(param.name)}_owned))",
            )
        if value_type.type_kind == TypeKind.STRUCT:
            struct = _find_struct(doc, value_type)
            if struct is None:
                raise AssertionError(f"missing struct {type_simple_name(value_type)}")
            object_name = f"{_sanitize_identifier(param.name)}_object"
            lines = self._generate_cpp_struct_to_js_object(
                doc,
                struct,
                value_name,
                object_name,
                indent="    ",
            )
            return lines, object_name

        mapped = _map_type(value_type)
        if mapped is None:
            raise AssertionError(f"unsupported out value type {type_simple_name(value_type)}")
        del method
        return [], self._cpp_return_expression(value_name, mapped)

    def _generate_cpp_binary_buffer_out_value(
        self,
        param: ParameterDef,
    ) -> tuple[list[str], str]:
        value_name = f"{_sanitize_identifier(param.name)}_value"
        return [
            f"    if ({value_name} == nullptr) {{",
            f'        throw Napi::Error::New(env, "native method returned null {param.name}");',
            "    }",
        ], (
            "ConvertIDasBinaryBufferToBuffer("
            f"env, {value_name}, BinaryBufferOwnershipMode::AdoptOwned)"
        )

    def _generate_cpp_owned_interface_out_value(
        self,
        param: ParameterDef,
        value_type: TypeInfo,
        expression: str,
    ) -> tuple[list[str], str]:
        value_name = f"{_sanitize_identifier(param.name)}_value"
        owned_name = f"{_sanitize_identifier(param.name)}_owned"
        return [
            f"    if ({value_name} == nullptr) {{",
            f'        throw Napi::Error::New(env, "native method returned null {param.name}");',
            "    }",
            f"    auto {owned_name} = DAS::DasPtr<{type_simple_name(value_type)}>::Attach({value_name});",
        ], expression

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
                f"    const {_cpp_storage_type(param.type_info)} {name}_value = static_cast<{_cpp_storage_type(param.type_info)}>({name}_raw);"
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
                f"    const {_cpp_storage_type(param.type_info)} {name}_value = static_cast<{_cpp_storage_type(param.type_info)}>({name}_raw);"
            )
        elif mapped.category in {"int32", "enum", "das_result"}:
            lines.append(
                f"    const {_cpp_storage_type(param.type_info)} {name}_value = static_cast<{_cpp_storage_type(param.type_info)}>("
            )
            lines.append(f"        info[{index}].As<Napi::Number>().Int32Value());")
        elif mapped.category == "uint32":
            lines.append(
                f"    const {_cpp_storage_type(param.type_info)} {name}_value = static_cast<{_cpp_storage_type(param.type_info)}>("
            )
            lines.append(f"        info[{index}].As<Napi::Number>().Uint32Value());")
        elif mapped.category == "number":
            lines.append(
                f"    const {_cpp_storage_type(param.type_info)} {name}_value = static_cast<{_cpp_storage_type(param.type_info)}>("
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
        lines.append(
            '    exports.Set("startHostIpc", Napi::Function::New(env, startHostIpc));'
        )
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
            if interface.name == "IDasBase":
                continue
            director_name = self._director_name(interface.name)
            lines.append(
                f'    exports.Set("create{director_name}", Napi::Function::New(env, create{director_name}));'
            )
        for interface in doc.interfaces:
            for method in _interface_declared_methods(interface):
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
            "",
        ]
        if _doc_needs_binary_buffer_conversion(doc):
            lines.extend(
                [
                    "export interface Buffer extends Uint8Array {",
                    "  readonly buffer: ArrayBufferLike;",
                    "}",
                    "",
                ]
            )
        lines.extend(
            [
            "export type DasResult = number & { readonly __dasResultBrand?: unique symbol };",
            "export const DasResult: Readonly<{",
            ]
        )
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
                "export interface StartHostIpcOptions {",
                "  mainPid?: number;",
                "  connectUrl?: string;",
                "  packageRoot?: string;",
                "  wrapperPath?: string;",
                "  addonPath?: string;",
                "}",
                "export function startHostIpc(options: StartHostIpcOptions): DasResult;",
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
            lines.extend(self._generate_dts_struct(struct, doc))
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
        for interface in _interfaces_base_first(doc):
            lines.extend(self._generate_dts_interface(interface))
            lines.append("")
            lines.extend(self._generate_dts_director(interface, doc))
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

    def _generate_dts_struct(self, struct: StructDef, doc: IdlDocument) -> list[str]:
        lines = [f"export interface {struct.name} {{"]
        for field in struct.fields:
            lines.append(f"  {public_js_name(field.name)}: {_ts_type_for_struct_field(field.type_name, doc)};")
        lines.append("}")
        return lines

    def _generate_dts_interface(self, interface: InterfaceDef) -> list[str]:
        base = interface.base_interface or "IDasBase"
        extends = f" extends {base}" if interface.name != "IDasBase" else ""
        lines = [f"export class {interface.name}{extends} {{"]
        lines.append(f"  static from(base: IDasBase): {interface.name};")
        lines.append("  dispose(): void;")
        for method in _interface_declared_methods(interface):
            js_name = public_js_name(method.name)
            lines.append(
                f"  {js_name}({_method_ts_params(method)}): {_method_ts_return(method)};"
            )
        lines.append("}")
        return lines

    def _generate_dts_director(self, interface: InterfaceDef, doc: IdlDocument) -> list[str]:
        director_name = self._director_name(interface.name)
        callbacks_name = f"{director_name}Callbacks"
        lines = [f"export interface {callbacks_name} {{"]
        for method in _interface_methods_including_bases(interface, doc):
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
        for interface in _interfaces_base_first(doc):
            lines.extend(self._generate_js_interface(interface))
            lines.append("")
            lines.extend(self._generate_js_director(interface))
            lines.append("")

        lines.extend(
            [
                "module.exports = {",
                "  DasResult: native.DasResult,",
                "  DasException,",
                "  startHostIpc: native.startHostIpc,",
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
        for method in _interface_declared_methods(interface):
            js_name = public_js_name(method.name)
            native_name = f"{interface.name}_{js_name}"
            in_params = _method_in_params(method)
            if not in_params:
                lines.extend(
                    [
                        f"  {js_name}(...args) {{",
                        f"    return native.{native_name}(this._ensureAlive(), ...args);",
                        "  }",
                    ]
                )
                continue

            js_params = [public_js_name(param.name) for param in in_params]
            native_args = []
            for param, js_param in zip(in_params, js_params):
                if (
                    param.type_info.type_kind == TypeKind.INTERFACE
                    and param.type_info.is_pointer
                    and param.type_info.pointer_level == 1
                    and not _is_readonly_string_interface_pointer(param.type_info)
                ):
                    native_args.append(f"{js_param}._ensureAlive()")
                else:
                    native_args.append(js_param)
            arg_list = ", ".join(js_params)
            native_arg_list = ", ".join(["this._ensureAlive()", *native_args])
            lines.extend(
                [
                    f"  {js_name}({arg_list}) {{",
                    f"    return native.{native_name}({native_arg_list});",
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
                f"    super(native.create{director_name}(callbacks));",
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
