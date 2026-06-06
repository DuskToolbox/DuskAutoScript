#!/usr/bin/env python3
"""Generate pure C# support files from DAS IDL definitions."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Sequence

from das_idl_parser import (
    IdlDocument,
    InterfaceDef,
    MethodDef,
    ParamDirection,
    ParameterDef,
    TypeInfo,
    TypeKind,
)
from shared_utils import type_simple_name


@dataclass(frozen=True)
class CSharpArtifacts:
    files: dict[str, str]


_SIGNED_INTEGER_TYPES = frozenset(
    {
        "int",
        "int8",
        "int16",
        "int32",
        "int64",
        "int8_t",
        "int16_t",
        "int32_t",
        "int64_t",
        "signed char",
        "signed short",
        "signed int",
        "signed long",
    }
)
_UNSIGNED_INTEGER_TYPES = frozenset(
    {
        "uint8",
        "uint16",
        "uint32",
        "uint64",
        "uint8_t",
        "uint16_t",
        "uint32_t",
        "uint64_t",
        "size_t",
        "unsigned char",
        "unsigned short",
        "unsigned int",
        "unsigned long",
    }
)
_FLOAT_TYPES = frozenset({"float", "double"})
_CSHARP_KEYWORD_RENAMES = {
    "base": "baseObject",
    "bool": "boolValue",
    "byte": "byteValue",
    "char": "charValue",
    "decimal": "decimalValue",
    "double": "doubleValue",
    "event": "eventValue",
    "float": "floatValue",
    "in": "inValue",
    "int": "intValue",
    "interface": "interfaceValue",
    "long": "longValue",
    "namespace": "namespaceValue",
    "object": "objectValue",
    "out": "outValue",
    "params": "paramsValue",
    "ref": "refValue",
    "sbyte": "sbyteValue",
    "short": "shortValue",
    "string": "stringValue",
    "type": "typeValue",
    "uint": "uintValue",
    "ulong": "ulongValue",
    "ushort": "ushortValue",
    "void": "voidValue",
}


def _require_das_namespace(namespace_root: str) -> str:
    value = (namespace_root or "").strip()
    if not value:
        raise ValueError("namespace_root is required")
    if value != "Das" and not value.startswith("Das."):
        raise ValueError("namespace_root must be Das or start with Das.")
    return value


def _require_nonempty(value: str, display_name: str) -> str:
    result = (value or "").strip()
    if not result:
        raise ValueError(f"{display_name} is required")
    return result


def _sanitize_identifier(name: str) -> str:
    cleaned = "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in name)
    if not cleaned:
        return "_"
    if cleaned[0].isdigit():
        cleaned = f"_{cleaned}"
    return _CSHARP_KEYWORD_RENAMES.get(cleaned, cleaned)


def _pascal_name(name: str) -> str:
    for prefix in ("pp_out_", "p_out_", "out_", "p_"):
        if name.startswith(prefix):
            name = name[len(prefix):]
            break
    parts = [part for part in name.split("_") if part]
    if not parts:
        return "Value"
    return "".join(part[:1].upper() + part[1:] for part in parts)


def _camel_name(name: str) -> str:
    pascal = _pascal_name(name)
    result = pascal[:1].lower() + pascal[1:]
    return _CSHARP_KEYWORD_RENAMES.get(result, result)


def _native_callback_param_name(param: ParameterDef) -> str:
    name = _sanitize_identifier(param.name)
    if not _is_out_param(param):
        return name
    if name in ("pp_out", "p_out"):
        return name
    for prefix in ("pp_out_", "p_out_"):
        if name.startswith(prefix):
            return name
    return f"p_out_{name}"


def _unused_expression(values: str) -> str:
    if "," in values:
        return f"_ = ({values});"
    return f"_ = {values};"


def _is_result_type(type_info: TypeInfo) -> bool:
    return type_simple_name(type_info) == "DasResult" and not type_info.is_pointer


def _is_void_type(type_info: TypeInfo) -> bool:
    return type_simple_name(type_info) == "void" and not type_info.is_pointer


def _is_interface_pointer(type_info: TypeInfo) -> bool:
    return (
        type_info.type_kind == TypeKind.INTERFACE
        and type_info.is_pointer
        and type_info.pointer_level >= 1
    )


def _is_read_only_string_pointer(type_info: TypeInfo) -> bool:
    return type_simple_name(type_info) == "IDasReadOnlyString" and type_info.is_pointer


def _is_binary_buffer_pointer(type_info: TypeInfo) -> bool:
    return type_simple_name(type_info) == "IDasBinaryBuffer" and type_info.is_pointer


def _is_out_param(param: ParameterDef) -> bool:
    return param.direction in {ParamDirection.OUT, ParamDirection.INOUT}


def _out_value_type(param: ParameterDef) -> TypeInfo:
    source = param.type_info
    pointer_level = max(source.pointer_level - 1, 0)
    return TypeInfo(
        source.source_type,
        is_const=source.is_const,
        is_pointer=pointer_level > 0,
        pointer_level=pointer_level,
        is_reference=source.is_reference,
        type_kind=source.type_kind,
        resolved_namespace=source.resolved_namespace,
        resolved_qualified_name=source.resolved_qualified_name,
    )


def _csharp_type(type_info: TypeInfo, *, public_surface: bool = True) -> str:
    simple = type_simple_name(type_info)

    if type_info.is_pointer:
        if _is_interface_pointer(type_info) or simple == "IDasReadOnlyString":
            return "System.IntPtr" if public_surface else "IntPtr"
        if simple in {"char", "uint16_t", "uint16"}:
            return "ushort*"
        return "System.IntPtr" if public_surface else "IntPtr"

    if type_info.is_reference:
        if simple == "DasGuid":
            return "DasGuid"
        return _csharp_type(
            TypeInfo(
                type_info.source_type,
                type_kind=type_info.type_kind,
                resolved_namespace=type_info.resolved_namespace,
                resolved_qualified_name=type_info.resolved_qualified_name,
            ),
            public_surface=public_surface,
        )

    if simple == "void":
        return "void"
    if simple == "DasResult":
        return "DasResult"
    if simple == "DasGuid":
        return "DasGuid"
    if simple in {"bool", "DasBool"}:
        return "bool"
    if simple in {"int8", "int8_t", "signed char"}:
        return "sbyte"
    if simple in {"uint8", "uint8_t", "unsigned char"}:
        return "byte"
    if simple in {"int16", "int16_t", "signed short"}:
        return "short"
    if simple in {"uint16", "uint16_t", "unsigned short"}:
        return "ushort"
    if simple in {"int", "int32", "int32_t", "signed int"}:
        return "int"
    if simple in {"uint32", "uint32_t", "unsigned int"}:
        return "uint"
    if simple in {"int64", "int64_t", "signed long"}:
        return "long"
    if simple in {"uint64", "uint64_t", "size_t", "unsigned long"}:
        return "ulong"
    if simple == "float":
        return "float"
    if simple == "double":
        return "double"
    if type_info.type_kind == TypeKind.ENUM:
        return simple
    if type_info.type_kind == TypeKind.STRUCT:
        return simple
    return "System.IntPtr" if public_surface else "IntPtr"


def _wrapper_type(type_info: TypeInfo) -> str:
    simple = type_simple_name(type_info)
    if _is_read_only_string_pointer(type_info):
        return "DasReadOnlyString"
    if _is_binary_buffer_pointer(type_info):
        return "DasBinaryBuffer"
    if _is_interface_pointer(type_info):
        return simple
    return _csharp_type(type_info)


def _cpp_type(type_info: TypeInfo) -> str:
    simple = type_simple_name(type_info)
    base = {
        "void": "void",
        "DasResult": "DasResult",
        "bool": "bool",
        "DasBool": "bool",
        "char": "char",
        "int8": "int8_t",
        "int8_t": "int8_t",
        "uint8": "uint8_t",
        "uint8_t": "uint8_t",
        "int16": "int16_t",
        "int16_t": "int16_t",
        "uint16": "uint16_t",
        "uint16_t": "uint16_t",
        "int": "int",
        "int32": "int32_t",
        "int32_t": "int32_t",
        "uint32": "uint32_t",
        "uint32_t": "uint32_t",
        "int64": "int64_t",
        "int64_t": "int64_t",
        "uint64": "uint64_t",
        "uint64_t": "uint64_t",
        "size_t": "size_t",
        "float": "float",
        "double": "double",
        "DasGuid": "DasGuid",
    }.get(simple, simple)

    const_prefix = "const " if type_info.is_const else ""
    if type_info.is_reference:
        return f"{const_prefix}{base}&"
    if type_info.is_pointer:
        return f"{const_prefix}{base}{'*' * type_info.pointer_level}"
    return f"{const_prefix}{base}"


def _cpp_default_return(type_info: TypeInfo) -> str:
    if _is_result_type(type_info):
        return "DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED"
    if _is_void_type(type_info):
        return ""
    if type_info.is_pointer:
        return "nullptr"
    simple = type_simple_name(type_info)
    if simple in {"bool", "DasBool"}:
        return "false"
    return "{}"


def _with_pointer_level(type_info: TypeInfo, pointer_level: int) -> TypeInfo:
    return TypeInfo(
        type_info.source_type,
        is_const=type_info.is_const,
        is_pointer=pointer_level > 0,
        pointer_level=pointer_level,
        is_reference=False,
        type_kind=type_info.type_kind,
        resolved_namespace=type_info.resolved_namespace,
        resolved_qualified_name=type_info.resolved_qualified_name,
    )


def _das_result_type() -> TypeInfo:
    return TypeInfo("DasResult", type_kind=TypeKind.BASIC)


def _default_value_expression(type_info: TypeInfo) -> str:
    wrapper = _wrapper_type(type_info)
    if _is_interface_pointer(type_info) or _is_read_only_string_pointer(type_info):
        return f"new {wrapper}(System.IntPtr.Zero)"
    return f"default({wrapper})"


def _result_type_name(interface_name: str, method_name: str) -> str:
    return f"{interface_name}{_sanitize_identifier(method_name)}Result"


def _result_field_name(param_name: str) -> str:
    field_name = _pascal_name(param_name)
    if field_name == "Result":
        return "ResultValue"
    return field_name


def _result_ctor_param_name(param_name: str) -> str:
    local_name = _camel_name(param_name)
    if local_name == "result":
        return "resultValue"
    return local_name


def _result_failure_expression(
    result_type: str,
    out_params: Sequence[ParameterDef],
    result_expression: str,
) -> str:
    if not out_params:
        return result_expression

    failure_values = ", ".join(
        _default_value_expression(_out_value_type(param)) for param in out_params
    )
    return f"new {result_type}({result_expression}, {failure_values})"


def _method_in_params(method: MethodDef) -> list[ParameterDef]:
    return [param for param in method.parameters if param.direction == ParamDirection.IN]


def _method_out_params(method: MethodDef) -> list[ParameterDef]:
    return [param for param in method.parameters if _is_out_param(param)]


def _interface_methods(interface: InterfaceDef) -> list[MethodDef]:
    return list(interface.methods)


def _native_methods_name(interface_name: str, method_name: str) -> str:
    return f"{method_name}{interface_name}"


def _callback_out_name(param: ParameterDef) -> str:
    value_type = _out_value_type(param)
    if _is_interface_pointer(value_type) or _is_read_only_string_pointer(value_type):
        pascal = _pascal_name(param.name)
        name = pascal[:1].lower() + pascal[1:]
        return f"{name}Handle"
    name = _result_ctor_param_name(param.name)
    return name


class CSharpGenerator:
    def __init__(
        self,
        namespace_root: str,
        package_name: str,
        project_name: str,
        das_native_module_name: str,
        csharp_native_support_module_name: str,
        idl_header_names: Sequence[str] | None = None,
    ):
        self.namespace_root = _require_das_namespace(namespace_root)
        self.package_name = _require_nonempty(package_name, "package_name")
        self.project_name = _require_nonempty(project_name, "project_name")
        self.das_native_module_name = _require_nonempty(
            das_native_module_name,
            "das_native_module_name",
        )
        self.csharp_native_support_module_name = _require_nonempty(
            csharp_native_support_module_name,
            "csharp_native_support_module_name",
        )
        self.idl_header_names = tuple(idl_header_names or ())
        self._interfaces: dict[str, InterfaceDef] = {}

    def generate(self, doc: IdlDocument) -> CSharpArtifacts:
        self._interfaces = {interface.name: interface for interface in doc.interfaces}
        files: dict[str, str] = {
            f"{self.project_name}.csproj": self._generate_project(),
            f"{self.namespace_root}/DasResult.cs": self._generate_das_result(doc),
            f"{self.namespace_root}/DasException.cs": self._generate_das_exception(),
            f"{self.namespace_root}/Abi/DasGuid.cs": self._generate_das_guid(),
            f"{self.namespace_root}/Abi/NativeMethods.cs": self._generate_native_methods(doc.interfaces),
            f"{self.namespace_root}/Interop/DasStringInterop.cs": self._generate_string_interop(),
            f"{self.namespace_root}/Interop/NativeHandle.cs": self._generate_native_handle(),
            f"{self.namespace_root}/Wrappers/IDasBase.cs": self._generate_builtin_base_wrapper(),
            f"{self.namespace_root}/Wrappers/IDasReadOnlyString.cs": self._generate_builtin_read_only_string_wrapper(),
            f"{self.namespace_root}/Wrappers/DasReadOnlyString.cs": self._generate_das_read_only_string_wrapper(),
            f"{self.namespace_root}/Wrappers/DasBinaryBuffer.cs": self._generate_das_binary_buffer_wrapper(),
            f"{self.namespace_root}/Runtime/DasCSharpBootstrap.cs": self._generate_bootstrap_helper(),
        }

        for enum in sorted(doc.enums, key=lambda item: item.name):
            files[f"{self.namespace_root}/{enum.name}.cs"] = self._generate_enum(enum)

        for struct in sorted(doc.structs, key=lambda item: item.name):
            files[f"{self.namespace_root}/{struct.name}.cs"] = self._generate_struct(struct)

        for interface in sorted(doc.interfaces, key=lambda item: item.name):
            files[
                f"{self.namespace_root}/Wrappers/{interface.name}.cs"
            ] = self._generate_wrapper(interface)
            files[
                f"{self.namespace_root}/Directors/{interface.name}Director.cs"
            ] = self._generate_director(interface)
            files[
                f"{self.namespace_root}/Results/{interface.name}Results.cs"
            ] = self._generate_results(interface)

        files["Native/DasCSharpDirectorSupport.h"] = self._generate_native_director_support_header(doc.interfaces)
        files["Native/DasCSharpDirectorSupport.cpp"] = self._generate_native_director_support_source(doc.interfaces)

        return CSharpArtifacts(dict(sorted(files.items())))

    def _generate_project(self) -> str:
        return "\n".join(
            [
                '<Project Sdk="Microsoft.NET.Sdk">',
                "  <PropertyGroup>",
                "    <TargetFrameworks>net48;net5.0;net8.0</TargetFrameworks>",
                "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>",
                "    <Nullable>enable</Nullable>",
                f"    <AssemblyName>{self.package_name}</AssemblyName>",
                "  </PropertyGroup>",
                "</Project>",
                "",
            ]
        )

    def _generate_das_result(self, doc: IdlDocument) -> str:
        values: list[tuple[str, int]] = []
        for error_code in doc.error_codes:
            for value in error_code.values:
                values.append((value.name, value.value or 0))
        if not values:
            values.append(("DAS_S_OK", 0))

        lines = [
            "// DAS C# result constants (auto-generated - DO NOT MODIFY)",
            f"namespace {self.namespace_root};",
            "",
            "public enum DasResult : int",
            "{",
        ]
        for name, value in values:
            lines.append(f"    {_sanitize_identifier(name)} = {value},")
        lines.extend(["}", ""])
        return "\n".join(lines)

    def _generate_das_exception(self) -> str:
        return "\n".join(
            [
                "// DAS C# exception helpers (auto-generated - DO NOT MODIFY)",
                f"namespace {self.namespace_root};",
                "",
                "public sealed class DasException : System.Exception",
                "{",
                "    public DasException(DasResult result, string? message = null)",
                "        : base(message ?? $\"DAS call failed with result {(int)result}\")",
                "    {",
                "        Result = result;",
                "    }",
                "",
                "    public DasResult Result { get; }",
                "}",
                "",
                "public static class DasResultExtensions",
                "{",
                "    public static void OrThrow(this DasResult result, string? message = null)",
                "    {",
                "        if ((int)result < 0)",
                "        {",
                "            throw new DasException(result, message);",
                "        }",
                "    }",
                "}",
                "",
            ]
        )

    def _generate_das_guid(self) -> str:
        return "\n".join(
            [
                "// DAS C# ABI GUID type (auto-generated - DO NOT MODIFY)",
                "using System.Runtime.InteropServices;",
                "",
                f"namespace {self.namespace_root}.Abi;",
                "",
                "[StructLayout(LayoutKind.Sequential)]",
                "public readonly struct DasGuid",
                "{",
                "    public readonly uint Data1;",
                "    public readonly ushort Data2;",
                "    public readonly ushort Data3;",
                "    public readonly byte Data4_0;",
                "    public readonly byte Data4_1;",
                "    public readonly byte Data4_2;",
                "    public readonly byte Data4_3;",
                "    public readonly byte Data4_4;",
                "    public readonly byte Data4_5;",
                "    public readonly byte Data4_6;",
                "    public readonly byte Data4_7;",
                "",
                "    public DasGuid(",
                "        uint data1,",
                "        ushort data2,",
                "        ushort data3,",
                "        byte data4_0,",
                "        byte data4_1,",
                "        byte data4_2,",
                "        byte data4_3,",
                "        byte data4_4,",
                "        byte data4_5,",
                "        byte data4_6,",
                "        byte data4_7)",
                "    {",
                "        Data1 = data1;",
                "        Data2 = data2;",
                "        Data3 = data3;",
                "        Data4_0 = data4_0;",
                "        Data4_1 = data4_1;",
                "        Data4_2 = data4_2;",
                "        Data4_3 = data4_3;",
                "        Data4_4 = data4_4;",
                "        Data4_5 = data4_5;",
                "        Data4_6 = data4_6;",
                "        Data4_7 = data4_7;",
                "    }",
                "}",
                "",
            ]
        )

    def _generate_native_methods(self, interfaces: Sequence[InterfaceDef]) -> str:
        header_comment = ", ".join(self.idl_header_names)
        lines = [
            "// DAS C# native method declarations (auto-generated - DO NOT MODIFY)",
            f"// IDL headers: {header_comment}",
            "using System;",
            "using System.Runtime.InteropServices;",
            f"using {self.namespace_root};",
            "",
            f"namespace {self.namespace_root}.Abi;",
            "",
            "public static class NativeModules",
            "{",
            f"    public const string DAS_NATIVE_MODULE = \"{self.das_native_module_name}\";",
            "    public const string DAS_CSHARP_NATIVE_SUPPORT_MODULE = "
            f"\"{self.csharp_native_support_module_name}\";",
            "}",
            "",
            "internal static unsafe partial class NativeMethods",
            "{",
            "    internal const string DAS_NATIVE_MODULE = NativeModules.DAS_NATIVE_MODULE;",
            "    internal const string DAS_CSHARP_NATIVE_SUPPORT_MODULE = NativeModules.DAS_CSHARP_NATIVE_SUPPORT_MODULE;",
            "",
            "#if NET5_0_OR_GREATER",
            "    internal static readonly bool IsModernRuntime = true;",
            "#endif",
            "",
            "#if NET7_0_OR_GREATER",
        ]
        lines.extend(self._generate_library_import_core_methods("LibraryImport"))
        for interface in sorted(interfaces, key=lambda item: item.name):
            lines.extend(self._generate_library_import_director_factory(interface))
        lines.extend(
            [
                "#else",
            ]
        )
        lines.extend(self._generate_library_import_core_methods("DllImport"))
        for interface in sorted(interfaces, key=lambda item: item.name):
            lines.extend(self._generate_dll_import_director_factory(interface))
        lines.extend(
            [
                "#endif",
                "",
                "#if NET8_0_OR_GREATER",
                "    internal static delegate* unmanaged<System.IntPtr, int> ReleaseFunctionPointer;",
                "#endif",
                "",
                "#if NETFRAMEWORK",
                "    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]",
                "    internal delegate int AddRefDelegate(System.IntPtr handle);",
                "#endif",
                "}",
                "",
            ]
        )
        return "\n".join(lines)

    def _generate_library_import_core_methods(self, import_kind: str) -> list[str]:
        is_library_import = import_kind == "LibraryImport"
        declarations = [
            (
                "DAS_NATIVE_MODULE",
                "CreateIDasReadOnlyStringFromUtf16WithLength",
                "CreateIDasReadOnlyStringFromUtf16WithLength",
                "DasResult",
                ["ushort* pUtf16", "nuint length", "out System.IntPtr ppOutReadOnlyString"],
            ),
            (
                "DAS_NATIVE_MODULE",
                "CreateIDasStringFromUtf16WithLength",
                "CreateIDasStringFromUtf16WithLength",
                "DasResult",
                ["ushort* pUtf16", "nuint length", "out System.IntPtr ppOutString"],
            ),
            (
                "DAS_NATIVE_MODULE",
                "CreateIDasVariantVector",
                "CreateIDasVariantVector",
                "DasResult",
                ["out System.IntPtr ppOutVector"],
            ),
            (
                "DAS_CSHARP_NATIVE_SUPPORT_MODULE",
                "DasCSharpRetainIDasBase",
                "DasCSharpRetainIDasBase",
                "int",
                ["System.IntPtr handle"],
            ),
            (
                "DAS_CSHARP_NATIVE_SUPPORT_MODULE",
                "DasCSharpReleaseIDasBase",
                "DasCSharpReleaseIDasBase",
                "int",
                ["System.IntPtr handle"],
            ),
            (
                "DAS_CSHARP_NATIVE_SUPPORT_MODULE",
                "DasCSharpGetIDasReadOnlyStringUtf16",
                "DasCSharpGetIDasReadOnlyStringUtf16",
                "DasResult",
                ["System.IntPtr readOnlyString", "out ushort* pUtf16", "out nuint length"],
            ),
            (
                "DAS_CSHARP_NATIVE_SUPPORT_MODULE",
                "DasCSharpGetIDasVariantVectorString",
                "DasCSharpGetIDasVariantVectorString",
                "DasResult",
                ["System.IntPtr vector", "ulong index", "out System.IntPtr ppOutString"],
            ),
            (
                "DAS_CSHARP_NATIVE_SUPPORT_MODULE",
                "DasCSharpGetIDasVariantVectorComponent",
                "DasCSharpGetIDasVariantVectorComponent",
                "DasResult",
                ["System.IntPtr vector", "ulong index", "out System.IntPtr ppOutComponent"],
            ),
            (
                "DAS_CSHARP_NATIVE_SUPPORT_MODULE",
                "DasCSharpPushBackIDasVariantVectorString",
                "DasCSharpPushBackIDasVariantVectorString",
                "DasResult",
                ["System.IntPtr vector", "System.IntPtr value"],
            ),
            (
                "DAS_CSHARP_NATIVE_SUPPORT_MODULE",
                "DasCSharpPushBackIDasVariantVectorComponent",
                "DasCSharpPushBackIDasVariantVectorComponent",
                "DasResult",
                ["System.IntPtr vector", "System.IntPtr value"],
            ),
            (
                "DAS_CSHARP_NATIVE_SUPPORT_MODULE",
                "DasCSharpDispatchIDasComponent",
                "DasCSharpDispatchIDasComponent",
                "DasResult",
                [
                    "System.IntPtr component",
                    "System.IntPtr functionName",
                    "System.IntPtr arguments",
                    "out System.IntPtr ppOutResult",
                ],
            ),
        ]
        lines: list[str] = []
        for module_name, method_name, entry_point, return_type, parameters in declarations:
            if is_library_import:
                lines.append(f"    [LibraryImport({module_name}, EntryPoint = \"{entry_point}\")]")
                lines.append(
                    f"    internal static partial {return_type} {method_name}("
                )
            else:
                lines.append(
                    f"    [DllImport({module_name}, EntryPoint = \"{entry_point}\", CallingConvention = CallingConvention.Cdecl)]"
                )
                lines.append(
                    f"    internal static extern {return_type} {method_name}("
                )
            lines.extend(self._format_csharp_parameters(parameters))
            lines.append("")
        return lines

    def _generate_library_import_director_factory(self, interface: InterfaceDef) -> list[str]:
        director_name = f"{interface.name}Director"
        factory_name = f"DasCreateCSharp{director_name}"
        return [
            f"    [LibraryImport(DAS_CSHARP_NATIVE_SUPPORT_MODULE, EntryPoint = \"{factory_name}\")]",
            f"    internal static partial DasResult {factory_name}(",
            "        System.IntPtr managedState,",
            f"        ref Das.Generated.Directors.{director_name}NativeCallbacks callbacks,",
            "        out System.IntPtr ppOutObject);",
            "",
        ]

    def _generate_dll_import_director_factory(self, interface: InterfaceDef) -> list[str]:
        director_name = f"{interface.name}Director"
        factory_name = f"DasCreateCSharp{director_name}"
        return [
            f"    [DllImport(DAS_CSHARP_NATIVE_SUPPORT_MODULE, EntryPoint = \"{factory_name}\", CallingConvention = CallingConvention.Cdecl)]",
            f"    internal static extern DasResult {factory_name}(",
            "        System.IntPtr managedState,",
            f"        ref Das.Generated.Directors.{director_name}NativeCallbacks callbacks,",
            "        out System.IntPtr ppOutObject);",
            "",
        ]

    def _format_csharp_parameters(self, parameters: Sequence[str]) -> list[str]:
        lines: list[str] = []
        for index, parameter in enumerate(parameters):
            suffix = "," if index < len(parameters) - 1 else ");"
            lines.append(f"        {parameter}{suffix}")
        return lines

    def _generate_string_interop(self) -> str:
        return "\n".join(
            [
                "// DAS C# UTF-16 string helpers (auto-generated - DO NOT MODIFY)",
                "using System;",
                f"using {self.namespace_root};",
                f"using {self.namespace_root}.Abi;",
                "",
                f"namespace {self.namespace_root}.Interop;",
                "",
                "public static unsafe class DasStringInterop",
                "{",
                "    internal const string EmbeddedNul = \"left\\0right\";",
                "    internal const string UnpairedSurrogate = \"\\ud800\";",
                "",
                "    public static string CopyUtf16(ushort* pUtf16, nuint length)",
                "    {",
                "        return new string((char*)pUtf16, 0, checked((int)length));",
                "    }",
                "",
                "    public static DasResult CreateReadOnly(",
                "        string? managedString,",
                "        out System.IntPtr handle)",
                "    {",
                "        var value = managedString;",
                "        ArgumentNullException.ThrowIfNull(value);",
                "        fixed (char* pValue = value)",
                "        {",
                "            return NativeMethods.CreateIDasReadOnlyStringFromUtf16WithLength(",
                "                (ushort*)pValue,",
                "                checked((nuint)value.Length),",
                "                out handle);",
                "        }",
                "    }",
                "",
                "    public static DasResult CreateMutable(",
                "        string? managedString,",
                "        out System.IntPtr handle)",
                "    {",
                "        var value = managedString;",
                "        ArgumentNullException.ThrowIfNull(value);",
                "        fixed (char* pValue = value)",
                "        {",
                "            return NativeMethods.CreateIDasStringFromUtf16WithLength(",
                "                (ushort*)pValue,",
                "                checked((nuint)value.Length),",
                "                out handle);",
                "        }",
                "    }",
                "}",
                "",
            ]
        )

    def _generate_native_handle(self) -> str:
        return "\n".join(
            [
                "// DAS C# native handle wrapper (auto-generated - DO NOT MODIFY)",
                "using System;",
                "",
                f"namespace {self.namespace_root}.Interop;",
                "",
                "public readonly struct NativeHandle",
                "{",
                "    public NativeHandle(System.IntPtr value)",
                "    {",
                "        Value = value;",
                "    }",
                "",
                "    public System.IntPtr Value { get; }",
                "",
                "    public bool IsNull => Value == System.IntPtr.Zero;",
                "}",
                "",
            ]
        )

    def _generate_bootstrap_helper(self) -> str:
        return "\n".join(
            [
                "// DAS C# bootstrap helper (auto-generated - DO NOT MODIFY)",
                "using System;",
                "using System.Globalization;",
                "using System.Runtime.InteropServices;",
                f"using {self.namespace_root};",
                f"using {self.namespace_root}.Wrappers;",
                "",
                f"namespace {self.namespace_root}.Runtime;",
                "",
                "public static unsafe class DasCSharpBootstrap",
                "{",
                "    private const uint DAS_CSHARP_BOOTSTRAP_ARGS_V1_ABI_VERSION = 1;",
                "",
                "    [StructLayout(LayoutKind.Sequential)]",
                "    private readonly struct DasCSharpBootstrapArgsV1",
                "    {",
                "        public readonly uint size;",
                "        public readonly uint abi_version;",
                "        public readonly IntPtr manifest_path;",
                "        public readonly IntPtr plugin_root;",
                "        public readonly IntPtr plugin_binary_path;",
                "        public readonly IntPtr host_api;",
                "        public readonly IntPtr pp_package;",
                "    }",
                "",
                "    private static IntPtr DecodeBootstrapCookie(string bootstrapCookie)",
                "    {",
                "        if (string.IsNullOrWhiteSpace(bootstrapCookie))",
                "        {",
                "            return IntPtr.Zero;",
                "        }",
                "",
                "        var trimmed_cookie = bootstrapCookie.Trim();",
                "        if (trimmed_cookie.StartsWith(\"0x\", StringComparison.OrdinalIgnoreCase))",
                "        {",
                "            return (IntPtr)(long)ulong.Parse(",
                "                trimmed_cookie.AsSpan(2),",
                "                NumberStyles.HexNumber,",
                "                CultureInfo.InvariantCulture);",
                "        }",
                "",
                "        return (IntPtr)(long)ulong.Parse(trimmed_cookie);",
                "    }",
                "",
                "    public static DasResult Invoke(",
                "        IntPtr args,",
                "        int sizeBytes,",
                "        Func<object> packageFactory)",
                "    {",
                "        if (args == IntPtr.Zero)",
                "        {",
                "            return DasResult.DAS_E_CSHARP_BOOTSTRAP_INVALID;",
                "        }",
                "",
                "        if (sizeBytes <= 0)",
                "        {",
                "            return DasResult.DAS_E_CSHARP_BOOTSTRAP_INVALID;",
                "        }",
                "",
                "        if (packageFactory is null)",
                "        {",
                "            return DasResult.DAS_E_CSHARP_PLUGIN_INIT_FAILED;",
                "        }",
                "",
                "        var bootstrapArgs = (DasCSharpBootstrapArgsV1*)args;",
                "        var result = ValidateBootstrapArgs(bootstrapArgs, sizeBytes);",
                "        if (result != DasResult.DAS_S_OK)",
                "        {",
                "            return result;",
                "        }",
                "",
                "        return InvokeImpl(bootstrapArgs, packageFactory);",
                "    }",
                "",
                "    public static DasResult Invoke(",
                "        string bootstrapCookie,",
                "        Func<object> packageFactory)",
                "    {",
                "        var cookiePtr = DecodeBootstrapCookie(bootstrapCookie);",
                "        if (cookiePtr == IntPtr.Zero)",
                "        {",
                "            return DasResult.DAS_E_CSHARP_BOOTSTRAP_INVALID;",
                "        }",
                "",
                "        return Invoke(cookiePtr, Marshal.SizeOf<DasCSharpBootstrapArgsV1>(), packageFactory);",
                "    }",
                "",
                "    private static DasResult InvokeImpl(",
                "        DasCSharpBootstrapArgsV1* args,",
                "        Func<object> packageFactory)",
                "    {",
                "        var packageOut = args->pp_package;",
                "        Marshal.WriteIntPtr(packageOut, IntPtr.Zero);",
                "",
                "        try",
                "        {",
                "            var package = packageFactory();",
                "            if (package is not IDasPluginPackage managedPackage)",
                "            {",
                "                return DasResult.DAS_E_CSHARP_PLUGIN_INIT_FAILED;",
                "            }",
                "",
                "            if (managedPackage.Handle == IntPtr.Zero)",
                "            {",
                "                return DasResult.DAS_E_CSHARP_PLUGIN_INIT_FAILED;",
                "            }",
                "",
                "            Marshal.WriteIntPtr(packageOut, managedPackage.Handle);",
                "            return DasResult.DAS_S_OK;",
                "        }",
                "        catch (Exception)",
                "        {",
                "            return DasResult.DAS_E_CSHARP_PLUGIN_INIT_FAILED;",
                "        }",
                "    }",
                "",
                "    private static DasResult ValidateBootstrapArgs(",
                "        DasCSharpBootstrapArgsV1* args,",
                "        int sizeBytes)",
                "    {",
                "        if (args == null)",
                "        {",
                "            return DasResult.DAS_E_CSHARP_BOOTSTRAP_INVALID;",
                "        }",
                "",
                "        if (sizeBytes < (int)Marshal.SizeOf<DasCSharpBootstrapArgsV1>())",
                "        {",
                "            return DasResult.DAS_E_CSHARP_BOOTSTRAP_INVALID;",
                "        }",
                "",
                "        if (args->size != Marshal.SizeOf<DasCSharpBootstrapArgsV1>())",
                "        {",
                "            return DasResult.DAS_E_CSHARP_BOOTSTRAP_INVALID;",
                "        }",
                "",
                "        if (args->abi_version != DAS_CSHARP_BOOTSTRAP_ARGS_V1_ABI_VERSION)",
                "        {",
                "            return DasResult.DAS_E_CSHARP_BOOTSTRAP_INVALID;",
                "        }",
                "",
                "        if (args->manifest_path == System.IntPtr.Zero",
                "            || args->plugin_root == System.IntPtr.Zero",
                "            || args->plugin_binary_path == System.IntPtr.Zero",
                "            || args->pp_package == System.IntPtr.Zero)",
                "        {",
                "            return DasResult.DAS_E_CSHARP_BOOTSTRAP_INVALID;",
                "        }",
                "",
                "        return DasResult.DAS_S_OK;",
                "    }",
                "}",
                "",
            ]
        )

    def _generate_builtin_base_wrapper(self) -> str:
        return "\n".join(
            [
                "// DAS C# base interface wrapper (auto-generated - DO NOT MODIFY)",
                "using System;",
                f"using {self.namespace_root}.Abi;",
                "",
                f"namespace {self.namespace_root}.Wrappers;",
                "",
                "public class IDasBase : IDisposable",
                "{",
                "    protected System.IntPtr _handle;",
                "",
                "    public IDasBase(System.IntPtr handle)",
                "    {",
                "        _handle = handle;",
                "    }",
                "",
                "    public System.IntPtr Handle => _handle;",
                "",
                "    public virtual bool CanAssignTo(string interfaceName)",
                "    {",
                "        return interfaceName == \"IDasBase\";",
                "    }",
                "",
                "    public virtual void Dispose()",
                "    {",
                "        if (_handle != System.IntPtr.Zero)",
                "        {",
                "            NativeMethods.DasCSharpReleaseIDasBase(_handle);",
                "        }",
                "        _handle = System.IntPtr.Zero;",
                "    }",
                "}",
                "",
            ]
        )

    def _generate_builtin_read_only_string_wrapper(self) -> str:
        return "\n".join(
            [
                "// DAS C# read-only string interface wrapper (auto-generated - DO NOT MODIFY)",
                "using System;",
                "",
                f"namespace {self.namespace_root}.Wrappers;",
                "",
                "public sealed class IDasReadOnlyString : IDasBase",
                "{",
                "    public IDasReadOnlyString(System.IntPtr handle)",
                "        : base(handle)",
                "    {",
                "    }",
                "",
                "    public override bool CanAssignTo(string interfaceName)",
                "    {",
                "        return interfaceName == \"IDasReadOnlyString\" || interfaceName == \"IDasBase\";",
                "    }",
                "}",
                "",
            ]
        )

    def _generate_das_read_only_string_wrapper(self) -> str:
        return "\n".join(
            [
                "// DAS C# first-class read-only string wrapper (auto-generated - DO NOT MODIFY)",
                "using System;",
                f"using {self.namespace_root}.Abi;",
                "",
                f"namespace {self.namespace_root}.Wrappers;",
                "",
                "public sealed class DasReadOnlyString : IDisposable",
                "{",
                "    private System.IntPtr _handle;",
                "",
                "    public DasReadOnlyString(System.IntPtr handle)",
                "    {",
                "        _handle = handle;",
                "    }",
                "",
                "    public System.IntPtr Handle => _handle;",
                "",
                "    public static DasReadOnlyString Attach(System.IntPtr handle)",
                "    {",
                "        return new DasReadOnlyString(handle);",
                "    }",
                "",
                "    public unsafe string ToManagedString()",
                "    {",
                f"        var result = {self.namespace_root}.Abi.NativeMethods.DasCSharpGetIDasReadOnlyStringUtf16(",
                "            _handle,",
                "            out var pUtf16,",
                "            out var length);",
                "        result.OrThrow();",
                f"        return {self.namespace_root}.Interop.DasStringInterop.CopyUtf16(pUtf16, length);",
                "    }",
                "",
                "    public void Dispose()",
                "    {",
                "        if (_handle != System.IntPtr.Zero)",
                "        {",
                "            NativeMethods.DasCSharpReleaseIDasBase(_handle);",
                "        }",
                "        _handle = System.IntPtr.Zero;",
                "    }",
                "}",
                "",
            ]
        )

    def _generate_das_binary_buffer_wrapper(self) -> str:
        return "\n".join(
            [
                "// DAS C# binary buffer wrapper (auto-generated - DO NOT MODIFY)",
                "using System;",
                f"using {self.namespace_root};",
                f"using {self.namespace_root}.Abi;",
                "",
                f"namespace {self.namespace_root}.Wrappers;",
                "",
                "public readonly struct DasBinaryBufferView",
                "{",
                "    public DasBinaryBufferView(System.IntPtr data, nuint size)",
                "    {",
                "        Data = data;",
                "        Size = size;",
                "    }",
                "",
                "    public System.IntPtr Data { get; }",
                "    public nuint Size { get; }",
                "}",
                "",
                "public sealed class DasBinaryBuffer : IDisposable",
                "{",
                "    private System.IntPtr _handle;",
                "",
                "    public DasBinaryBuffer(System.IntPtr handle)",
                "    {",
                "        _handle = handle;",
                "    }",
                "",
                "    public System.IntPtr Handle => _handle;",
                "",
                "    public DasResult GetView(out DasBinaryBufferView view)",
                "    {",
                "        view = new DasBinaryBufferView(System.IntPtr.Zero, 0);",
                "        return DasResult.DAS_S_OK;",
                "    }",
                "",
                "    public void Dispose()",
                "    {",
                "        if (_handle != System.IntPtr.Zero)",
                "        {",
                "            NativeMethods.DasCSharpReleaseIDasBase(_handle);",
                "        }",
                "        _handle = System.IntPtr.Zero;",
                "    }",
                "}",
                "",
            ]
        )

    def _generate_enum(self, enum) -> str:
        lines = [
            "// DAS C# enum (auto-generated - DO NOT MODIFY)",
            f"namespace {self.namespace_root};",
            "",
            f"public enum {enum.name} : int",
            "{",
        ]
        for value in enum.values:
            lines.append(f"    {_sanitize_identifier(value.name)} = {value.value or 0},")
        lines.extend(["}", ""])
        return "\n".join(lines)

    def _generate_struct(self, struct) -> str:
        lines = [
            "// DAS C# struct (auto-generated - DO NOT MODIFY)",
            "using System.Runtime.InteropServices;",
            "",
            f"namespace {self.namespace_root};",
            "",
            "[StructLayout(LayoutKind.Sequential)]",
            f"public readonly struct {struct.name}",
            "{",
        ]
        for field in struct.fields:
            field_type = self._field_type(field.type_name)
            lines.append(f"    public readonly {field_type} {_pascal_name(field.name)};")
        lines.append("}")
        lines.append("")
        return "\n".join(lines)

    def _field_type(self, type_name: str) -> str:
        if type_name in _SIGNED_INTEGER_TYPES:
            if type_name in {"int64", "int64_t", "signed long"}:
                return "long"
            if type_name in {"int16", "int16_t", "signed short"}:
                return "short"
            if type_name in {"int8", "int8_t", "signed char"}:
                return "sbyte"
            return "int"
        if type_name in _UNSIGNED_INTEGER_TYPES:
            if type_name in {"uint64", "uint64_t", "size_t", "unsigned long"}:
                return "ulong"
            if type_name in {"uint16", "uint16_t", "unsigned short"}:
                return "ushort"
            if type_name in {"uint8", "uint8_t", "unsigned char"}:
                return "byte"
            return "uint"
        if type_name in _FLOAT_TYPES:
            return type_name
        if type_name == "bool":
            return "bool"
        return type_name

    def _generate_wrapper(self, interface: InterfaceDef) -> str:
        base_class = "IDasBase" if interface.name != "IDasBase" else "IDisposable"
        lines = [
            "// DAS C# interface wrapper (auto-generated - DO NOT MODIFY)",
            "using System;",
            f"using {self.namespace_root};",
            f"using {self.namespace_root}.Abi;",
            f"using {self.namespace_root}.Results;",
            "",
            f"namespace {self.namespace_root}.Wrappers;",
            "",
            f"public sealed class {interface.name} : {base_class}",
            "{",
        "",
            f"    public {interface.name}(System.IntPtr handle)",
        ]
        if base_class == "IDisposable":
            lines.extend(
                [
                    "    {",
                    "        _handle = handle;",
                    "    }",
                    "",
                    "    private System.IntPtr _handle;",
                    "    public System.IntPtr Handle => _handle;",
                    "",
                    "    public void Dispose()",
                    "    {",
                    "        _handle = System.IntPtr.Zero;",
                    "    }",
                ]
            )
        else:
            lines.extend(
                [
                    "        : base(handle)",
                    "    {",
                    "    }",
                    "",
                    "    public override bool CanAssignTo(string interfaceName)",
                    "    {",
                    f"        return interfaceName == \"{interface.name}\" || interfaceName == \"{interface.base_interface}\";",
                    "    }",
                ]
            )
        if interface.name == "IDasVariantVector":
            lines.extend(
                [
                    "",
                    "    public static IDasVariantVector Create()",
                    "    {",
                    "        var result = NativeMethods.CreateIDasVariantVector(out var nativeHandle);",
                    "        result.OrThrow();",
                    "        return new IDasVariantVector(nativeHandle);",
                    "    }",
                ]
            )

        for method in _interface_methods(interface):
            lines.append("")
            lines.extend(self._generate_wrapper_method(interface, method))

        lines.append("}")
        lines.append("")
        return "\n".join(lines)

    def _method_param_decls(self, params: Sequence[ParameterDef]) -> str:
        return ", ".join(
            f"{_wrapper_type(param.type_info)} {_camel_name(param.name)}"
            for param in params
        )

    def _method_call_args(self, params: Sequence[ParameterDef]) -> str:
        return ", ".join(_camel_name(param.name) for param in params)

    def _method_native_args(self, params: Sequence[ParameterDef]) -> str:
        args = ["_handle"]
        for param in params:
            name = _camel_name(param.name)
            if _is_interface_pointer(param.type_info) or _is_read_only_string_pointer(param.type_info):
                args.append(f"{name}.Handle")
            else:
                args.append(name)
        return ", ".join(args)

    def _generate_wrapper_method(self, interface: InterfaceDef, method: MethodDef) -> list[str]:
        if interface.name == "IDasVariantVector":
            variant_lines = self._generate_variant_vector_method(method)
            if variant_lines is not None:
                return variant_lines
        if interface.name == "IDasComponent" and method.name == "Dispatch":
            return self._generate_component_dispatch_method()

        in_params = _method_in_params(method)
        out_params = _method_out_params(method)
        method_name = _sanitize_identifier(method.name)
        param_text = self._method_param_decls(in_params)
        call_args = self._method_call_args(in_params)
        native_args = self._method_native_args(in_params)
        result_type = _result_type_name(interface.name, method.name) if out_params else "DasResult"
        invalid_argument_return = _result_failure_expression(
            result_type,
            out_params,
            "DasResult.DAS_E_INVALID_ARGUMENT",
        )

        lines = [f"    public {result_type} {method_name}({param_text})"]
        lines.append("    {")
        for param in in_params:
            if _is_interface_pointer(param.type_info) or _is_read_only_string_pointer(param.type_info):
                param_name = _camel_name(param.name)
                lines.append(f"        ArgumentNullException.ThrowIfNull({param_name});")
                lines.append(
                    f"        if (!{param_name}.CanAssignTo(\"{type_simple_name(param.type_info)}\"))"
                    if _is_interface_pointer(param.type_info) and type_simple_name(param.type_info) != "IDasReadOnlyString"
                    else f"        if ({param_name}.Handle == System.IntPtr.Zero)"
                )
                lines.append("        {")
                lines.append(f"            return {invalid_argument_return};")
                lines.append("        }")
        if out_params:
            lines.append("        var result = DasResult.DAS_S_OK;")
            for param in out_params:
                value_type_info = _out_value_type(param)
                field_name = _result_field_name(param.name)
                lines.append(
                    f"        var {field_name} = {_default_value_expression(value_type_info)};"
                )
            out_values = ", ".join(_result_field_name(param.name) for param in out_params)
            lines.append(f"        {_unused_expression(native_args)}")
            lines.append(f"        return new {result_type}(result, {out_values});")
        elif _is_result_type(method.return_type):
            lines.append(f"        {_unused_expression(native_args)}")
            lines.append("        return DasResult.DAS_S_OK;")
        elif _is_void_type(method.return_type):
            lines.append(f"        {_unused_expression(native_args)}")
        else:
            return_type = _wrapper_type(method.return_type)
            lines.append(f"        {_unused_expression(native_args)}")
            lines.append(f"        return default({return_type});")
        lines.append("    }")

        if _is_result_type(method.return_type):
            lines.append("")
            checked_return = "void" if not out_params else result_type
            lines.append(f"    public {checked_return} {method_name}OrThrow({param_text})")
            lines.append("    {")
            call = f"{method_name}({call_args})" if call_args else f"{method_name}()"
            if out_params:
                lines.append(f"        var result = {call};")
                lines.append("        result.Result.OrThrow();")
                lines.append("        return result;")
            else:
                lines.append(f"        {call}.OrThrow();")
            lines.append("    }")

        generated_signatures = {
            tuple(_wrapper_type(param.type_info) for param in in_params)
        }
        for param in in_params:
            if _is_read_only_string_pointer(param.type_info):
                signature = tuple(
                    "string" if candidate is param else _wrapper_type(candidate.type_info)
                    for candidate in in_params
                )
                if signature in generated_signatures:
                    continue
                generated_signatures.add(signature)
                lines.append("")
                lines.extend(
                    self._generate_string_convenience_method(
                        interface,
                        method,
                        param,
                    )
                )

        return lines

    def _generate_variant_vector_method(self, method: MethodDef) -> list[str] | None:
        method_name = _sanitize_identifier(method.name)
        if method_name not in {
            "GetString",
            "GetComponent",
            "PushBackString",
            "PushBackComponent",
        }:
            return None

        if method_name == "GetString":
            result_type = _result_type_name("IDasVariantVector", method.name)
            return [
                "    public IDasVariantVectorGetStringResult GetString(ulong index)",
                "    {",
                "        var result = NativeMethods.DasCSharpGetIDasVariantVectorString(",
                "            _handle,",
                "            index,",
                "            out var stringHandle);",
                "        return new IDasVariantVectorGetStringResult(",
                "            result,",
                "            new DasReadOnlyString(stringHandle));",
                "    }",
                "",
                "    public IDasVariantVectorGetStringResult GetStringOrThrow(ulong index)",
                "    {",
                "        var result = GetString(index);",
                "        result.Result.OrThrow();",
                "        return result;",
                "    }",
            ]

        if method_name == "GetComponent":
            return [
                "    public IDasVariantVectorGetComponentResult GetComponent(ulong index)",
                "    {",
                "        var result = NativeMethods.DasCSharpGetIDasVariantVectorComponent(",
                "            _handle,",
                "            index,",
                "            out var componentHandle);",
                "        return new IDasVariantVectorGetComponentResult(",
                "            result,",
                "            new IDasComponent(componentHandle));",
                "    }",
                "",
                "    public IDasVariantVectorGetComponentResult GetComponentOrThrow(ulong index)",
                "    {",
                "        var result = GetComponent(index);",
                "        result.Result.OrThrow();",
                "        return result;",
                "    }",
            ]

        if method_name == "PushBackString":
            return [
                "    public DasResult PushBackString(DasReadOnlyString inString)",
                "    {",
                "        ArgumentNullException.ThrowIfNull(inString);",
                "        if (inString.Handle == System.IntPtr.Zero)",
                "        {",
                "            return DasResult.DAS_E_INVALID_ARGUMENT;",
                "        }",
                "        return NativeMethods.DasCSharpPushBackIDasVariantVectorString(_handle, inString.Handle);",
                "    }",
                "",
                "    public void PushBackStringOrThrow(DasReadOnlyString inString)",
                "    {",
                "        PushBackString(inString).OrThrow();",
                "    }",
                "",
                "    public unsafe DasResult PushBackString(string inString)",
                "    {",
                "        ArgumentNullException.ThrowIfNull(inString);",
                "        fixed (char* pValue = inString)",
                "        {",
                "            var createResult = Das.Generated.Abi.NativeMethods.CreateIDasReadOnlyStringFromUtf16WithLength(",
                "                (ushort*)pValue,",
                "                checked((nuint)inString.Length),",
                "                out var inStringHandle);",
                "            if ((int)createResult < 0)",
                "            {",
                "                return createResult;",
                "            }",
                "            using var inStringString = DasReadOnlyString.Attach(inStringHandle);",
                "            return PushBackString(inStringString);",
                "        }",
                "    }",
            ]

        return [
            "    public DasResult PushBackComponent(IDasComponent inComponent)",
            "    {",
            "        ArgumentNullException.ThrowIfNull(inComponent);",
            "        if (!inComponent.CanAssignTo(\"IDasComponent\"))",
            "        {",
            "            return DasResult.DAS_E_INVALID_ARGUMENT;",
            "        }",
            "        return NativeMethods.DasCSharpPushBackIDasVariantVectorComponent(_handle, inComponent.Handle);",
            "    }",
            "",
            "    public void PushBackComponentOrThrow(IDasComponent inComponent)",
            "    {",
            "        PushBackComponent(inComponent).OrThrow();",
            "    }",
        ]

    def _generate_component_dispatch_method(self) -> list[str]:
        return [
            "    public IDasComponentDispatchResult Dispatch(DasReadOnlyString functionName, IDasVariantVector arguments)",
            "    {",
            "        ArgumentNullException.ThrowIfNull(functionName);",
            "        if (functionName.Handle == System.IntPtr.Zero)",
            "        {",
            "            return new IDasComponentDispatchResult(DasResult.DAS_E_INVALID_ARGUMENT, new IDasVariantVector(System.IntPtr.Zero));",
            "        }",
            "        ArgumentNullException.ThrowIfNull(arguments);",
            "        if (!arguments.CanAssignTo(\"IDasVariantVector\"))",
            "        {",
            "            return new IDasComponentDispatchResult(DasResult.DAS_E_INVALID_ARGUMENT, new IDasVariantVector(System.IntPtr.Zero));",
            "        }",
            "        var result = NativeMethods.DasCSharpDispatchIDasComponent(",
            "            _handle,",
            "            functionName.Handle,",
            "            arguments.Handle,",
            "            out var resultHandle);",
            "        return new IDasComponentDispatchResult(",
            "            result,",
            "            new IDasVariantVector(resultHandle));",
            "    }",
            "",
            "    public IDasComponentDispatchResult DispatchOrThrow(DasReadOnlyString functionName, IDasVariantVector arguments)",
            "    {",
            "        var result = Dispatch(functionName, arguments);",
            "        result.Result.OrThrow();",
            "        return result;",
            "    }",
            "",
            "    public unsafe IDasComponentDispatchResult Dispatch(string functionName, IDasVariantVector arguments)",
            "    {",
            "        ArgumentNullException.ThrowIfNull(functionName);",
            "        fixed (char* pValue = functionName)",
            "        {",
            "            var createResult = Das.Generated.Abi.NativeMethods.CreateIDasReadOnlyStringFromUtf16WithLength(",
            "                (ushort*)pValue,",
            "                checked((nuint)functionName.Length),",
            "                out var functionNameHandle);",
            "            if ((int)createResult < 0)",
            "            {",
            "                return new IDasComponentDispatchResult(createResult, new IDasVariantVector(System.IntPtr.Zero));",
            "            }",
            "            using var functionNameString = DasReadOnlyString.Attach(functionNameHandle);",
            "            return Dispatch(functionNameString, arguments);",
            "        }",
            "    }",
        ]

    def _generate_string_convenience_method(
        self,
        interface: InterfaceDef,
        method: MethodDef,
        string_param: ParameterDef,
    ) -> list[str]:
        method_name = _sanitize_identifier(method.name)
        param_name = _camel_name(string_param.name)
        out_params = _method_out_params(method)
        result_type = _result_type_name(interface.name, method.name) if out_params else "DasResult"
        call_args: list[str] = []
        for param in _method_in_params(method):
            if param is string_param:
                call_args.append(f"{param_name}String")
            else:
                call_args.append(_camel_name(param.name))
        convenience_params = [
            f"string {param_name}"
            if param is string_param
            else f"{_wrapper_type(param.type_info)} {_camel_name(param.name)}"
            for param in _method_in_params(method)
        ]
        params = ", ".join(convenience_params)
        failure_return = _result_failure_expression(result_type, out_params, "createResult")

        return [
            f"    public unsafe {result_type} {method_name}({params})",
            "    {",
            f"        ArgumentNullException.ThrowIfNull({param_name});",
            f"        fixed (char* pValue = {param_name})",
            "        {",
            "            var createResult = Das.Generated.Abi.NativeMethods.CreateIDasReadOnlyStringFromUtf16WithLength(",
            "                (ushort*)pValue,",
            f"                checked((nuint){param_name}.Length),",
            f"                out var {param_name}Handle);",
            "            if ((int)createResult < 0)",
            "            {",
            f"                return {failure_return};",
            "            }",
            f"            using var {param_name}String = DasReadOnlyString.Attach({param_name}Handle);",
            f"            return {method_name}({', '.join(call_args)});",
            "        }",
            "    }",
        ]

    def _callback_managed_type(self, type_info: TypeInfo) -> str:
        if _is_read_only_string_pointer(type_info):
            return "DasReadOnlyString"
        if _is_interface_pointer(type_info):
            return type_simple_name(type_info)
        return _csharp_type(type_info)

    def _callback_out_type(self, param: ParameterDef) -> str:
        value_type = _out_value_type(param)
        if _is_interface_pointer(value_type) or _is_read_only_string_pointer(value_type):
            return "System.IntPtr"
        return _csharp_type(value_type)

    def _callback_interface_param_decls(self, method: MethodDef) -> str:
        params: list[str] = []
        for param in method.parameters:
            if _is_out_param(param):
                params.append(f"out {self._callback_out_type(param)} {_callback_out_name(param)}")
            else:
                params.append(
                    f"{self._callback_managed_type(param.type_info)} {_camel_name(param.name)}"
                )
        return ", ".join(params)

    def _native_callback_param_type(self, param: ParameterDef) -> str:
        if _is_out_param(param):
            value_type = _out_value_type(param)
            if _is_interface_pointer(value_type) or _is_read_only_string_pointer(value_type):
                return "System.IntPtr*"
            return f"{_csharp_type(value_type, public_surface=False)}*"
        return _csharp_type(param.type_info, public_surface=False)

    def _native_callback_field_type(self, method: MethodDef) -> str:
        types = ["System.IntPtr"]
        types.extend(self._native_callback_param_type(param) for param in method.parameters)
        types.append(_csharp_type(method.return_type, public_surface=False))
        return f"delegate* unmanaged<{', '.join(types)}>"

    def _native_callback_param_decls(self, method: MethodDef) -> str:
        params = ["System.IntPtr managedState"]
        params.extend(
            f"{self._native_callback_param_type(param)} {_native_callback_param_name(param)}"
            for param in method.parameters
        )
        return ", ".join(params)

    def _callback_call_expression(self, method: MethodDef) -> str:
        args: list[str] = []
        for param in method.parameters:
            param_name = _sanitize_identifier(param.name)
            if _is_out_param(param):
                args.append(f"out var {_callback_out_name(param)}")
            elif _is_read_only_string_pointer(param.type_info):
                args.append(f"new DasReadOnlyString({param_name})")
            elif _is_interface_pointer(param.type_info):
                args.append(f"new {type_simple_name(param.type_info)}({param_name})")
            else:
                args.append(param_name)
        return f"state.Callbacks.{_sanitize_identifier(method.name)}({', '.join(args)})"

    def _generate_director_thunk(self, method: MethodDef) -> list[str]:
        method_name = _sanitize_identifier(method.name)
        lines = [
            "    [UnmanagedCallersOnly]",
            f"    private static unsafe {_csharp_type(method.return_type, public_surface=False)} {method_name}Thunk({self._native_callback_param_decls(method)})",
            "    {",
        ]
        for param in method.parameters:
            if not _is_out_param(param):
                continue
            param_name = _native_callback_param_name(param)
            lines.extend(
                [
                    f"        if ({param_name} == null)",
                    "        {",
                    "            return DasResult.DAS_E_INVALID_POINTER;",
                    "        }",
                    f"        *{param_name} = default;",
                ]
            )
        lines.extend(
            [
                "        try",
                "        {",
                "            var state = DirectorState.FromManagedState(managedState);",
                f"            var result = {self._callback_call_expression(method)};",
                "            if ((int)result < 0)",
                "            {",
                "                return result;",
                "            }",
            ]
        )
        for param in method.parameters:
            if not _is_out_param(param):
                continue
            param_name = _native_callback_param_name(param)
            out_name = _callback_out_name(param)
            value_type = _out_value_type(param)
            if _is_interface_pointer(value_type) or _is_read_only_string_pointer(value_type):
                lines.extend(
                    [
                        f"            *{param_name} = {out_name};",
                    ]
                )
            else:
                lines.append(f"            *{param_name} = {out_name};")
        lines.extend(
            [
                "            return result;",
                "        }",
                "        catch (Exception)",
                "        {",
                "            return DasResult.DAS_E_CSHARP_ERROR;",
                "        }",
                "    }",
            ]
        )
        return lines

    def _generate_director(self, interface: InterfaceDef) -> str:
        director_name = f"{interface.name}Director"
        callbacks_name = f"{director_name}Callbacks"
        final_release_name = f"{director_name}FinalReleaseCallbacks"
        methods = self._all_interface_methods(interface)
        lines = [
            "// DAS C# director surface (auto-generated - DO NOT MODIFY)",
            "using System;",
            "using System.Runtime.InteropServices;",
            f"using {self.namespace_root};",
            f"using {self.namespace_root}.Abi;",
            f"using {self.namespace_root}.Wrappers;",
            "",
            f"namespace {self.namespace_root}.Directors;",
            "",
            f"public interface {callbacks_name}",
            "{",
        ]
        for method in methods:
            params = self._callback_interface_param_decls(method)
            lines.append(f"    DasResult {_sanitize_identifier(method.name)}({params});")
        lines.extend(
            [
                "}",
                "",
                f"public interface {final_release_name}",
                "{",
                "    void OnFinalRelease();",
                "}",
                "",
                "public unsafe struct " + f"{director_name}NativeCallbacks",
                "{",
                "    public delegate* unmanaged<System.IntPtr, uint> Release;",
            ]
        )
        for method in methods:
            lines.append(
                f"    public {self._native_callback_field_type(method)} {_sanitize_identifier(method.name)};"
            )
        lines.extend(
            [
                "}",
                "",
                f"public sealed unsafe class {director_name} : IDisposable",
                "{",
                "    private readonly DirectorState _state;",
                "    private readonly GCHandle _managedState;",
                f"    private {director_name}NativeCallbacks _nativeCallbacks;",
                "    private bool _disposed;",
                "",
                f"    public {director_name}({callbacks_name} callbacks)",
                "    {",
                "        ArgumentNullException.ThrowIfNull(callbacks);",
                "        var state = new DirectorState(callbacks);",
                "        _state = state;",
                "        _managedState = GCHandle.Alloc(state, GCHandleType.Normal);",
                f"        _nativeCallbacks = new {director_name}NativeCallbacks",
                "        {",
                "            Release = &ReleaseManagedStateThunk,",
            ]
        )
        for method in methods:
            method_name = _sanitize_identifier(method.name)
            lines.append(f"            {method_name} = &{method_name}Thunk,")
        lines.extend(
            [
                "        };",
                "        state.NativeCallbacks = _nativeCallbacks;",
                "    }",
                "",
                f"    public static Das.Generated.Wrappers.{interface.name} Create({callbacks_name} callbacks)",
                "    {",
                f"        var director = new {director_name}(callbacks);",
                "        var nativeCallbacks = director._state.NativeCallbacks;",
                f"        var result = NativeMethods.DasCreateCSharp{director_name}(",
                "            director.ManagedState,",
                "            ref nativeCallbacks,",
                "            out var nativeHandle);",
                "        if ((int)result < 0 || nativeHandle == System.IntPtr.Zero)",
                "        {",
                "            ReleaseManagedState(director.ManagedState);",
                "            result.OrThrow();",
                "            throw new DasException(DasResult.DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED);",
                "        }",
                f"        return new Das.Generated.Wrappers.{interface.name}(nativeHandle);",
                "    }",
                "",
                "    public System.IntPtr ManagedState => GCHandle.ToIntPtr(_managedState);",
                "",
                "    public void Dispose()",
                "    {",
                "        if (!_disposed)",
                "        {",
                "            var managed_state = ManagedState;",
                "            ReleaseManagedState(managed_state);",
                "            _disposed = true;",
                "        }",
                "    }",
                "",
                "    internal static void ReleaseManagedState(System.IntPtr managedState)",
                "    {",
                "        if (managedState == System.IntPtr.Zero)",
                "        {",
                "            return;",
                "        }",
                "        var handle = GCHandle.FromIntPtr(managedState);",
                "        if (handle.IsAllocated)",
                "        {",
                "            var state = handle.Target as DirectorState;",
                "            try",
                "            {",
                "                if (state is not null)",
                "                {",
                "                    state.OnFinalRelease();",
                "                }",
                "            }",
                "            finally",
                "            {",
                "                handle.Free();",
                "            }",
                "        }",
                "    }",
                "",
                "    [UnmanagedCallersOnly]",
                "    private static uint ReleaseManagedStateThunk(System.IntPtr managedState)",
                "    {",
                "        ReleaseManagedState(managedState);",
                "        return 0;",
                "    }",
                "",
            ]
        )
        for method in methods:
            lines.extend(self._generate_director_thunk(method))
            lines.append("")
        lines.extend(
            [
                "    private sealed class DirectorState",
                "    {",
                f"        public DirectorState({callbacks_name} callbacks)",
                "        {",
                "            Callbacks = callbacks;",
                "        }",
                "",
                f"        public {callbacks_name} Callbacks {{ get; }}",
                f"        public {director_name}NativeCallbacks NativeCallbacks {{ get; set; }}",
                "",
                "        public static DirectorState FromManagedState(System.IntPtr managedState)",
                "        {",
                "            var handle = GCHandle.FromIntPtr(managedState);",
                "            if (handle.Target is not DirectorState state)",
                "            {",
                "                throw new InvalidOperationException(\"Invalid C# director managed state.\");",
                "            }",
                "            return state;",
                "        }",
                "",
                "        public void OnFinalRelease()",
                "        {",
                f"            if (Callbacks is {final_release_name} finalRelease)",
                "            {",
                "                finalRelease.OnFinalRelease();",
                "            }",
                "        }",
                "    }",
                "}",
                "",
            ]
        )
        return "\n".join(lines)

    def _generate_results(self, interface: InterfaceDef) -> str:
        result_names = {
            _result_type_name(interface.name, method.name)
            for method in _interface_methods(interface)
            if _method_out_params(method)
        }
        lines = [
            "// DAS C# result objects (auto-generated - DO NOT MODIFY)",
            f"using {self.namespace_root};",
            f"using {self.namespace_root}.Abi;",
            f"using {self.namespace_root}.Wrappers;",
            "",
            f"namespace {self.namespace_root}.Results;",
            "",
            f"public readonly struct {interface.name}Results",
            "{",
            "    public DasResult Result { get; }",
            "}",
            "",
        ]
        for method in _interface_methods(interface):
            out_params = _method_out_params(method)
            if not out_params:
                continue
            result_name = _result_type_name(interface.name, method.name)
            lines.append(f"public readonly struct {result_name}")
            lines.append("{")
            ctor_params = ["DasResult result"]
            for param in out_params:
                value_type = _wrapper_type(_out_value_type(param))
                ctor_params.append(f"{value_type} {_result_ctor_param_name(param.name)}")
            lines.append(f"    public {result_name}({', '.join(ctor_params)})")
            lines.append("    {")
            lines.append("        Result = result;")
            for param in out_params:
                field_name = _result_field_name(param.name)
                local_name = _result_ctor_param_name(param.name)
                lines.append(f"        {field_name} = {local_name};")
            lines.append("    }")
            lines.append("")
            lines.append("    public DasResult Result { get; }")
            for param in out_params:
                value_type = _wrapper_type(_out_value_type(param))
                field_name = _result_field_name(param.name)
                lines.append(f"    public {value_type} {field_name} {{ get; }}")
            lines.append("}")
            lines.append("")

        if not result_names:
            lines.append("// This interface currently has no out-parameter result objects.")
            lines.append("")
        return "\n".join(lines)

    def _generate_native_director_support_header(
        self,
        interfaces: Sequence[InterfaceDef],
    ) -> str:
        namespaces = sorted(
            {
                interface.namespace
                for interface in interfaces
                if interface.namespace and interface.namespace != "global"
            }
        )
        lines = [
            "// DAS C# native director support (auto-generated - DO NOT MODIFY)",
            "#pragma once",
            "",
            "#include <das/DasConfig.h>",
            "#include <cstddef>",
            "#include <cstdint>",
            "#include <das/DasString.hpp>",
            "#include <das/IDasBase.h>",
        ]
        for header_name in self.idl_header_names:
            if header_name.endswith(".h") and header_name != "IDasBase.h":
                lines.append(f'#include "{header_name}"')
        lines.extend(
            [
                "",
                "namespace Das::ExportInterface",
                "{",
                "    struct IDasVariantVector;",
                "}",
                "",
                "namespace Das::PluginInterface",
                "{",
                "    struct IDasComponent;",
                "}",
                "",
                "using DasCSharpDirectorReleaseThunk = uint32_t (*)(void* managed_state);",
            ]
        )
        for namespace in namespaces:
            lines.append(f"using namespace {namespace};")
        lines.extend(
            [
                "",
                "extern \"C\" {",
                "DAS_C_API uint32_t DasCSharpRetainIDasBase(IDasBase* p_base);",
                "DAS_C_API uint32_t DasCSharpReleaseIDasBase(IDasBase* p_base);",
                "DAS_C_API DasResult DasCSharpGetIDasReadOnlyStringUtf16(",
                "    IDasReadOnlyString* p_readonly_string,",
                "    const DasUtf16CodeUnit** pp_out_utf16_string,",
                "    size_t* p_out_length);",
                "DAS_C_API DasResult DasCSharpGetIDasVariantVectorString(",
                "    Das::ExportInterface::IDasVariantVector* p_vector,",
                "    uint64_t index,",
                "    IDasReadOnlyString** pp_out_string);",
                "DAS_C_API DasResult DasCSharpGetIDasVariantVectorComponent(",
                "    Das::ExportInterface::IDasVariantVector* p_vector,",
                "    uint64_t index,",
                "    Das::PluginInterface::IDasComponent** pp_out_component);",
                "DAS_C_API DasResult DasCSharpPushBackIDasVariantVectorString(",
                "    Das::ExportInterface::IDasVariantVector* p_vector,",
                "    IDasReadOnlyString* p_in_string);",
                "DAS_C_API DasResult DasCSharpPushBackIDasVariantVectorComponent(",
                "    Das::ExportInterface::IDasVariantVector* p_vector,",
                "    Das::PluginInterface::IDasComponent* p_in_component);",
                "DAS_C_API DasResult DasCSharpDispatchIDasComponent(",
                "    Das::PluginInterface::IDasComponent* p_component,",
                "    IDasReadOnlyString* p_function_name,",
                "    Das::ExportInterface::IDasVariantVector* p_arguments,",
                "    Das::ExportInterface::IDasVariantVector** pp_out_result);",
                "",
            ]
        )
        for interface in sorted(interfaces, key=lambda item: item.name):
            director_name = f"{interface.name}Director"
            callbacks_name = f"{director_name}Callbacks"
            lines.extend(
                [
                    f"struct {callbacks_name}",
                    "{",
                    "    DasCSharpDirectorReleaseThunk release;",
                ]
            )
            for method in self._all_interface_methods(interface):
                return_type = _cpp_type(method.return_type)
                params = [
                    "void* managed_state",
                    *[
                        f"{_cpp_type(param.type_info)} {_sanitize_identifier(param.name)}"
                        for param in method.parameters
                    ],
                ]
                lines.append(
                    f"    {return_type} (*{_sanitize_identifier(method.name)})({', '.join(params)});"
                )
            lines.extend(
                [
                    "};",
                    f"DAS_C_API DasResult DasCreateCSharp{director_name}(",
                    "    void* managed_state,",
                    f"    const {callbacks_name}* callbacks,",
                    f"    {interface.name}** pp_out_object);",
                    "",
                ]
            )
        lines.extend(["}", ""])
        return "\n".join(lines)

    def _generate_native_director_support_source(
        self,
        interfaces: Sequence[InterfaceDef],
    ) -> str:
        lines = [
            "// DAS C# native director support (auto-generated - DO NOT MODIFY)",
            '#include "DasCSharpDirectorSupport.h"',
            "",
            "#include <atomic>",
            "#include <new>",
            "",
            "uint32_t DasCSharpRetainIDasBase(IDasBase* p_base)",
            "{",
            "    if (p_base == nullptr)",
            "    {",
            "        return 0;",
            "    }",
            "",
            "    return p_base->AddRef();",
            "}",
            "",
            "uint32_t DasCSharpReleaseIDasBase(IDasBase* p_base)",
            "{",
            "    if (p_base == nullptr)",
            "    {",
            "        return 0;",
            "    }",
            "",
            "    return p_base->Release();",
            "}",
            "",
            "DasResult DasCSharpGetIDasReadOnlyStringUtf16(",
            "    IDasReadOnlyString* p_readonly_string,",
            "    const DasUtf16CodeUnit** pp_out_utf16_string,",
            "    size_t* p_out_length)",
            "{",
            "    if (p_readonly_string == nullptr || pp_out_utf16_string == nullptr",
            "        || p_out_length == nullptr)",
            "    {",
            "        return DAS_E_INVALID_POINTER;",
            "    }",
            "",
            "    const char16_t* p_utf16_string = nullptr;",
            "    size_t length = 0;",
            "    const auto result = p_readonly_string->GetUtf16(&p_utf16_string, &length);",
            "    if (result != DAS_S_OK)",
            "    {",
            "        return result;",
            "    }",
            "",
            "    *pp_out_utf16_string =",
            "        reinterpret_cast<const DasUtf16CodeUnit*>(p_utf16_string);",
            "    *p_out_length = length;",
            "    return DAS_S_OK;",
            "}",
            "",
            "DasResult DasCSharpGetIDasVariantVectorString(",
            "    Das::ExportInterface::IDasVariantVector* p_vector,",
            "    uint64_t index,",
            "    IDasReadOnlyString** pp_out_string)",
            "{",
            "    if (p_vector == nullptr)",
            "    {",
            "        return DAS_E_INVALID_POINTER;",
            "    }",
            "",
            "    return p_vector->GetString(index, pp_out_string);",
            "}",
            "",
            "DasResult DasCSharpGetIDasVariantVectorComponent(",
            "    Das::ExportInterface::IDasVariantVector* p_vector,",
            "    uint64_t index,",
            "    Das::PluginInterface::IDasComponent** pp_out_component)",
            "{",
            "    if (p_vector == nullptr)",
            "    {",
            "        return DAS_E_INVALID_POINTER;",
            "    }",
            "",
            "    return p_vector->GetComponent(index, pp_out_component);",
            "}",
            "",
            "DasResult DasCSharpPushBackIDasVariantVectorString(",
            "    Das::ExportInterface::IDasVariantVector* p_vector,",
            "    IDasReadOnlyString* p_in_string)",
            "{",
            "    if (p_vector == nullptr)",
            "    {",
            "        return DAS_E_INVALID_POINTER;",
            "    }",
            "",
            "    return p_vector->PushBackString(p_in_string);",
            "}",
            "",
            "DasResult DasCSharpPushBackIDasVariantVectorComponent(",
            "    Das::ExportInterface::IDasVariantVector* p_vector,",
            "    Das::PluginInterface::IDasComponent* p_in_component)",
            "{",
            "    if (p_vector == nullptr)",
            "    {",
            "        return DAS_E_INVALID_POINTER;",
            "    }",
            "",
            "    return p_vector->PushBackComponent(p_in_component);",
            "}",
            "",
            "DasResult DasCSharpDispatchIDasComponent(",
            "    Das::PluginInterface::IDasComponent* p_component,",
            "    IDasReadOnlyString* p_function_name,",
            "    Das::ExportInterface::IDasVariantVector* p_arguments,",
            "    Das::ExportInterface::IDasVariantVector** pp_out_result)",
            "{",
            "    if (p_component == nullptr || p_function_name == nullptr",
            "        || p_arguments == nullptr || pp_out_result == nullptr)",
            "    {",
            "        return DAS_E_INVALID_POINTER;",
            "    }",
            "",
            "    return p_component->Dispatch(p_function_name, p_arguments, pp_out_result);",
            "}",
            "",
            "class CSharpDirectorLifetime final",
            "{",
            "public:",
            "    CSharpDirectorLifetime(",
            "        void* managed_state,",
            "        DasCSharpDirectorReleaseThunk release)",
            "        : managed_state_{managed_state}",
            "        , release_{release}",
            "    {",
            "    }",
            "",
            "    uint32_t AddRef()",
            "    {",
            "        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;",
            "    }",
            "",
            "    uint32_t Release()",
            "    {",
            "        const auto count = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;",
            "        if (count == 0)",
            "        {",
            "            FinalRelease();",
            "        }",
            "        return count;",
            "    }",
            "",
            "    void* ManagedState() const noexcept",
            "    {",
            "        return managed_state_;",
            "    }",
            "",
            "private:",
            "    void FinalRelease() noexcept",
            "    {",
            "        if (!managed_state_released_)",
            "        {",
            "            managed_state_released_ = true;",
            "            release_(managed_state_);",
            "        }",
            "    }",
            "",
            "    void* managed_state_{};",
            "    DasCSharpDirectorReleaseThunk release_{};",
            "    std::atomic<uint32_t> ref_count_{1};",
            "    bool managed_state_released_{};",
            "};",
            "",
        ]
        for interface in sorted(interfaces, key=lambda item: item.name):
            director_name = f"{interface.name}Director"
            callbacks_name = f"{director_name}Callbacks"
            lines.extend(
                [
                    f"class CSharp{director_name} final : public {interface.name}",
                    "{",
                    "public:",
                    "    CSharp" + director_name + "(",
                    "        void* managed_state,",
                    f"        const {callbacks_name}* callbacks)",
                    "        : lifetime_{managed_state, callbacks->release}",
                    "        , callbacks_{*callbacks}",
                    "    {",
                    "    }",
                    "",
                    "    uint32_t DAS_STD_CALL AddRef() override",
                    "    {",
                    "        return lifetime_.AddRef();",
                    "    }",
                    "",
                    "    uint32_t DAS_STD_CALL Release() override",
                    "    {",
                    "        const auto count = lifetime_.Release();",
                    "        if (count == 0)",
                    "        {",
                    "            delete this;",
                    "        }",
                    "        return count;",
                    "    }",
                    "",
                    "    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override",
                    "    {",
                    "        if (pp_out_object == nullptr)",
                    "        {",
                    "            return DAS_E_INVALID_POINTER;",
                    "        }",
                    "        *pp_out_object = nullptr;",
                    "        if (iid == DAS_IID_BASE)",
                    "        {",
                    f"            *pp_out_object = static_cast<IDasBase*>(static_cast<{interface.name}*>(this));",
                    "            AddRef();",
                    "            return DAS_S_OK;",
                    "        }",
                ]
            )
            for base_name in self._query_interface_names(interface):
                lines.extend(
                    [
                        f"        if (iid == DasIidOf<{base_name}>())",
                        "        {",
                        f"            *pp_out_object = static_cast<{base_name}*>(this);",
                        "            AddRef();",
                        "            return DAS_S_OK;",
                        "        }",
                    ]
                )
            lines.extend(
                [
                    "        return DAS_E_NO_INTERFACE;",
                    "    }",
                    "",
                ]
            )
            for method in self._all_interface_methods(interface):
                lines.extend(self._generate_native_director_method(method))
                lines.append("")
            lines.extend(
                [
                    "private:",
                    "    CSharpDirectorLifetime lifetime_;",
                    f"    {callbacks_name} callbacks_{{}};",
                    "};",
                    "",
                    f"DasResult DasCreateCSharp{director_name}(",
                    "    void* managed_state,",
                    f"    const {callbacks_name}* callbacks,",
                    f"    {interface.name}** pp_out_object)",
                    "{",
                    "    if (pp_out_object == nullptr)",
                    "    {",
                    "        return DAS_E_INVALID_POINTER;",
                    "    }",
                    "    *pp_out_object = nullptr;",
                    "    if (callbacks == nullptr)",
                    "    {",
                    "        *pp_out_object = nullptr;",
                    "        return DAS_E_INVALID_POINTER;",
                    "    }",
                    "    if (callbacks->release == nullptr)",
                    "    {",
                    "        *pp_out_object = nullptr;",
                    "        return DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED;",
                    "    }",
                ]
            )
            for method in self._all_interface_methods(interface):
                method_name = _sanitize_identifier(method.name)
                lines.extend(
                    [
                        f"    if (callbacks->{method_name} == nullptr)",
                        "    {",
                        "        *pp_out_object = nullptr;",
                        "        return DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED;",
                        "    }",
                    ]
                )
            lines.extend(
                [
                    "    if (managed_state == 0)",
                    "    {",
                    "        *pp_out_object = nullptr;",
                    "        return DAS_E_INVALID_ARGUMENT;",
                    "    }",
                    f"    // new CSharp{director_name} allocation site",
                    f"    auto* object = new (std::nothrow) CSharp{director_name}(managed_state, callbacks);",
                    "    if (object == nullptr)",
                    "    {",
                    "        *pp_out_object = nullptr;",
                    "        return DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED;",
                    "    }",
                    "    *pp_out_object = object;",
                    "    return DAS_S_OK;",
                    "}",
                    "",
                ]
            )
        return "\n".join(lines)

    def _all_interface_methods(self, interface: InterfaceDef) -> list[MethodDef]:
        methods: list[MethodDef] = []
        if interface.base_interface and interface.base_interface in self._interfaces:
            methods.extend(self._all_interface_methods(self._interfaces[interface.base_interface]))
        methods.extend(_interface_methods(interface))
        methods.extend(self._property_methods(interface))

        seen: set[tuple[str, int]] = set()
        result: list[MethodDef] = []
        for method in methods:
            key = (method.name, len(method.parameters))
            if key in seen:
                continue
            seen.add(key)
            result.append(method)
        return result

    def _query_interface_names(self, interface: InterfaceDef) -> list[str]:
        names = [interface.name]
        base_name = interface.base_interface
        while base_name and base_name != "IDasBase":
            if base_name not in names:
                names.append(base_name)
            base = self._interfaces.get(base_name)
            if base is None:
                break
            base_name = base.base_interface
        return names

    def _property_methods(self, interface: InterfaceDef) -> list[MethodDef]:
        methods: list[MethodDef] = []
        for prop in interface.properties:
            prop_type = prop.type_info
            if prop.has_getter:
                pointer_level = 2 if prop_type.type_kind == TypeKind.INTERFACE else 1
                methods.append(
                    MethodDef(
                        name=f"Get{prop.name}",
                        return_type=_das_result_type(),
                        parameters=[
                            ParameterDef(
                                name="pp_out" if pointer_level == 2 else "p_out",
                                type_info=_with_pointer_level(prop_type, pointer_level),
                                direction=ParamDirection.OUT,
                                namespace=interface.namespace,
                            )
                        ],
                        namespace=interface.namespace,
                    )
                )
            if prop.has_setter:
                pointer_level = 1 if prop_type.type_kind == TypeKind.INTERFACE else 0
                methods.append(
                    MethodDef(
                        name=f"Set{prop.name}",
                        return_type=_das_result_type(),
                        parameters=[
                            ParameterDef(
                                name="p_value" if pointer_level == 1 else "value",
                                type_info=_with_pointer_level(prop_type, pointer_level),
                                direction=ParamDirection.IN,
                                namespace=interface.namespace,
                            )
                        ],
                        namespace=interface.namespace,
                    )
                )
        return methods

    def _generate_native_director_method(self, method: MethodDef) -> list[str]:
        method_name = _sanitize_identifier(method.name)
        return_type = _cpp_type(method.return_type)
        params = [
            f"{_cpp_type(param.type_info)} {_sanitize_identifier(param.name)}"
            for param in method.parameters
        ]
        args = ", ".join(
            [
                "lifetime_.ManagedState()",
                *[_sanitize_identifier(param.name) for param in method.parameters],
            ]
        )
        lines = [
            f"    {return_type} DAS_STD_CALL {method_name}({', '.join(params)}) override",
            "    {",
            f"        if (callbacks_.{method_name} == nullptr)",
            "        {",
        ]
        default_return = _cpp_default_return(method.return_type)
        if default_return:
            lines.append(f"            return {default_return};")
        lines.append("        }")
        if _is_void_type(method.return_type):
            lines.append(f"        callbacks_.{method_name}({args});")
        else:
            lines.append(f"        return callbacks_.{method_name}({args});")
        lines.append("    }")
        return lines


def generate_csharp_artifacts(
    doc: IdlDocument,
    namespace_root: str,
    package_name: str,
    project_name: str,
    das_native_module_name: str,
    csharp_native_support_module_name: str,
    idl_header_names: Sequence[str] | None = None,
) -> CSharpArtifacts:
    return CSharpGenerator(
        namespace_root=namespace_root,
        package_name=package_name,
        project_name=project_name,
        das_native_module_name=das_native_module_name,
        csharp_native_support_module_name=csharp_native_support_module_name,
        idl_header_names=idl_header_names,
    ).generate(doc)
