#!/usr/bin/env python3
"""Generate Node/NAPI support files from DAS IDL definitions."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Sequence

from das_idl_parser import (
    IdlDocument,
    ModuleFunctionDef,
    ParameterDef,
    ParamDirection,
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
            "#include <string>",
            "",
            '#include "das/IDasBase.h"',
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
            "    Napi::Object result_constants = Napi::Object::New(env);",
        ]
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
            "export type DasResult = number & { readonly __dasResultBrand?: unique symbol };",
            "export const DasResult: Readonly<{",
        ]
        for error_code in doc.error_codes:
            for value in error_code.values:
                lines.append(f"  {value.name}: {value.value};")
        lines.extend(["}>;", ""])

        for enum in doc.enums:
            lines.append(f"export const {enum.name}: Readonly<{{")
            for value in enum.values:
                lines.append(f"  {value.name}: {value.value};")
            lines.append("}>;")
            lines.append("")

        lines.extend(
            [
                "export type DasGuid = string & { readonly __dasGuidBrand: unique symbol };",
                "export function guid(value: string): DasGuid;",
                "",
            ]
        )
        for interface in doc.interfaces:
            director_name = self._director_name(interface.name)
            lines.append(
                f"export class {director_name} {{"
            )
            lines.append("  /** director support is deferred to Phase 74 */")
            lines.append("  constructor(...args: never[]);")
            lines.append("}")
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
        ]
        for interface in doc.interfaces:
            director_name = self._director_name(interface.name)
            lines.append(f"class {director_name} {{")
            lines.append("  constructor() {")
            lines.append(
                f"    throw new Error('{director_name} director support is deferred to Phase 74');"
            )
            lines.append("  }")
            lines.append("}")
            lines.append("")

        lines.extend(
            [
                "module.exports = {",
                "  DasResult: native.DasResult,",
            ]
        )
        for enum in doc.enums:
            lines.append(f"  {enum.name}: native.{enum.name},")
        lines.append("  guid,")
        for interface in doc.interfaces:
            director_name = self._director_name(interface.name)
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
