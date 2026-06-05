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
        return f"_{cleaned}"
    return cleaned


def _pascal_name(name: str) -> str:
    for prefix in ("pp_out_", "p_out_", "out_", "p_"):
        if name.startswith(prefix):
            name = name[len(prefix):]
            break
    parts = [part for part in name.split("_") if part]
    if not parts:
        return "Value"
    return "".join(part[:1].upper() + part[1:] for part in parts)


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


def _method_in_params(method: MethodDef) -> list[ParameterDef]:
    return [param for param in method.parameters if param.direction == ParamDirection.IN]


def _method_out_params(method: MethodDef) -> list[ParameterDef]:
    return [param for param in method.parameters if _is_out_param(param)]


def _interface_methods(interface: InterfaceDef) -> list[MethodDef]:
    return list(interface.methods)


class CSharpGenerator:
    def __init__(
        self,
        namespace_root: str,
        package_name: str,
        project_name: str,
        idl_header_names: Sequence[str] | None = None,
    ):
        self.namespace_root = _require_das_namespace(namespace_root)
        self.package_name = _require_nonempty(package_name, "package_name")
        self.project_name = _require_nonempty(project_name, "project_name")
        self.idl_header_names = tuple(idl_header_names or ())

    def generate(self, doc: IdlDocument) -> CSharpArtifacts:
        files: dict[str, str] = {
            f"{self.project_name}.csproj": self._generate_project(),
            f"{self.namespace_root}/DasResult.cs": self._generate_das_result(doc),
            f"{self.namespace_root}/DasException.cs": self._generate_das_exception(),
            f"{self.namespace_root}/Abi/DasGuid.cs": self._generate_das_guid(),
            f"{self.namespace_root}/Abi/NativeMethods.cs": self._generate_native_methods(),
            f"{self.namespace_root}/Interop/DasStringInterop.cs": self._generate_string_interop(),
            f"{self.namespace_root}/Interop/NativeHandle.cs": self._generate_native_handle(),
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

    def _generate_native_methods(self) -> str:
        header_comment = ", ".join(self.idl_header_names)
        return "\n".join(
            [
                "// DAS C# native method declarations (auto-generated - DO NOT MODIFY)",
                f"// IDL headers: {header_comment}",
                "using System;",
                "using System.Runtime.InteropServices;",
                "",
                f"namespace {self.namespace_root}.Abi;",
                "",
                "internal static unsafe partial class NativeMethods",
                "{",
                "#if NET5_0_OR_GREATER",
                "    internal static readonly bool IsModernRuntime = true;",
                "#endif",
                "",
                "#if NET7_0_OR_GREATER",
                "    [LibraryImport(\"das\", EntryPoint = \"DasAddRef\")]",
                "    internal static partial int DasAddRef(System.IntPtr handle);",
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
                "",
                "    internal static partial DasResult CreateIDasReadOnlyStringFromUtf16WithLength(",
                "        ushort* pUtf16,",
                "        nuint length,",
                "        out System.IntPtr ppOutReadOnlyString);",
                "",
                "    internal static partial DasResult CreateIDasStringFromUtf16WithLength(",
                "        ushort* pUtf16,",
                "        nuint length,",
                "        out System.IntPtr ppOutString);",
                "}",
                "",
            ]
        )

    def _generate_string_interop(self) -> str:
        return "\n".join(
            [
                "// DAS C# UTF-16 string helpers (auto-generated - DO NOT MODIFY)",
                "using System;",
                f"using {self.namespace_root}.Abi;",
                "",
                f"namespace {self.namespace_root}.Interop;",
                "",
                "public static unsafe class DasStringInterop",
                "{",
                "    internal const string EmbeddedNul = \"left\\0right\";",
                "    internal const string UnpairedSurrogate = \"\\ud800\";",
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
        lines = [
            "// DAS C# interface wrapper (auto-generated - DO NOT MODIFY)",
            "using System;",
            "",
            f"namespace {self.namespace_root}.Wrappers;",
            "",
            f"public sealed class {interface.name} : IDisposable",
            "{",
            "    private System.IntPtr _handle;",
            "",
            f"    public {interface.name}(System.IntPtr handle)",
            "    {",
            "        _handle = handle;",
            "    }",
            "",
            "    public System.IntPtr Handle => _handle;",
            "",
            "    public void Dispose()",
            "    {",
            "        _handle = System.IntPtr.Zero;",
            "    }",
        ]

        for method in _interface_methods(interface):
            lines.append("")
            lines.extend(self._generate_wrapper_method(method))

        lines.append("}")
        lines.append("")
        return "\n".join(lines)

    def _generate_wrapper_method(self, method: MethodDef) -> list[str]:
        in_params = _method_in_params(method)
        out_params = _method_out_params(method)
        method_name = _sanitize_identifier(method.name)
        param_text = ", ".join(
            f"{_csharp_type(param.type_info)} {_sanitize_identifier(param.name)}"
            for param in in_params
        )
        call_args = ", ".join(_sanitize_identifier(param.name) for param in in_params)
        native_args = ", ".join(item for item in ("_handle", call_args) if item)
        result_type = f"{method_name}Result" if out_params else "DasResult"

        lines = [f"    public {result_type} {method_name}({param_text})"]
        lines.append("    {")
        if out_params:
            lines.append("        var result = DasResult.DAS_S_OK;")
            for param in out_params:
                value_type = _csharp_type(_out_value_type(param))
                field_name = _pascal_name(param.name)
                lines.append(f"        var {field_name} = default({value_type});")
            out_values = ", ".join(_pascal_name(param.name) for param in out_params)
            lines.append(f"        return new {method_name}Result(result, {out_values});")
        elif _is_result_type(method.return_type):
            lines.append(f"        _ = {native_args};")
            lines.append("        return DasResult.DAS_S_OK;")
        elif _is_void_type(method.return_type):
            lines.append(f"        _ = {native_args};")
        else:
            return_type = _csharp_type(method.return_type)
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

        for param in in_params:
            if _is_read_only_string_pointer(param.type_info):
                lines.append("")
                lines.extend(self._generate_string_convenience_method(method, param))

        return lines

    def _generate_string_convenience_method(
        self,
        method: MethodDef,
        string_param: ParameterDef,
    ) -> list[str]:
        method_name = _sanitize_identifier(method.name)
        param_name = _sanitize_identifier(string_param.name)
        value_name = _pascal_name(string_param.name)
        convenience_name = f"{method_name}{value_name}String"
        call_args: list[str] = []
        for param in _method_in_params(method):
            if param is string_param:
                call_args.append(f"{param_name}Handle")
            else:
                call_args.append(_sanitize_identifier(param.name))
        extra_params = [
            f"{_csharp_type(param.type_info)} {_sanitize_identifier(param.name)}"
            for param in _method_in_params(method)
            if param is not string_param
        ]
        params = ", ".join(["string? managedString", *extra_params])

        return [
            f"    public DasResult {convenience_name}({params})",
            "    {",
            "        var value = managedString;",
            "        ArgumentNullException.ThrowIfNull(value);",
            f"        var createResult = {self.namespace_root}.Interop.DasStringInterop.CreateReadOnly(",
            "            value,",
            f"            out var {param_name}Handle);",
            "        if ((int)createResult < 0)",
            "        {",
            "            return createResult;",
            "        }",
            f"        return {method_name}({', '.join(call_args)});",
            "    }",
        ]

    def _generate_director(self, interface: InterfaceDef) -> str:
        director_name = f"{interface.name}Director"
        callbacks_name = f"{director_name}Callbacks"
        lines = [
            "// DAS C# director surface (auto-generated - DO NOT MODIFY)",
            "using System;",
            "using System.Runtime.InteropServices;",
            "",
            f"namespace {self.namespace_root}.Directors;",
            "",
            f"public interface {callbacks_name}",
            "{",
        ]
        for method in _interface_methods(interface):
            params = ", ".join(
                f"{_csharp_type(param.type_info)} {_sanitize_identifier(param.name)}"
                for param in _method_in_params(method)
            )
            lines.append(f"    DasResult {_sanitize_identifier(method.name)}({params});")
        lines.extend(
            [
                "}",
                "",
                f"public sealed class {director_name} : IDisposable",
                "{",
                "    private readonly GCHandle _managedState;",
                "",
                f"    public {director_name}({callbacks_name} callbacks)",
                "    {",
                "        ArgumentNullException.ThrowIfNull(callbacks);",
                "        _managedState = GCHandle.Alloc(callbacks, GCHandleType.Normal);",
                "    }",
                "",
                "    public System.IntPtr ManagedState => GCHandle.ToIntPtr(_managedState);",
                "",
                "    public void Dispose()",
                "    {",
                "        if (_managedState.IsAllocated)",
                "        {",
                "            _managedState.Free();",
                "        }",
                "    }",
                "}",
                "",
            ]
        )
        return "\n".join(lines)

    def _generate_results(self, interface: InterfaceDef) -> str:
        result_names = {
            f"{_sanitize_identifier(method.name)}Result"
            for method in _interface_methods(interface)
            if _method_out_params(method)
        }
        lines = [
            "// DAS C# result objects (auto-generated - DO NOT MODIFY)",
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
            result_name = f"{_sanitize_identifier(method.name)}Result"
            lines.append(f"public readonly struct {result_name}")
            lines.append("{")
            ctor_params = ["DasResult result"]
            for param in out_params:
                value_type = _csharp_type(_out_value_type(param))
                field_name = _pascal_name(param.name)
                ctor_params.append(f"{value_type} {field_name[:1].lower() + field_name[1:]}")
            lines.append(f"    public {result_name}({', '.join(ctor_params)})")
            lines.append("    {")
            lines.append("        Result = result;")
            for param in out_params:
                field_name = _pascal_name(param.name)
                local_name = field_name[:1].lower() + field_name[1:]
                lines.append(f"        {field_name} = {local_name};")
            lines.append("    }")
            lines.append("")
            lines.append("    public DasResult Result { get; }")
            for param in out_params:
                value_type = _csharp_type(_out_value_type(param))
                field_name = _pascal_name(param.name)
                lines.append(f"    public {value_type} {field_name} {{ get; }}")
            lines.append("}")
            lines.append("")

        if not result_names:
            lines.append("// This interface currently has no out-parameter result objects.")
            lines.append("")
        return "\n".join(lines)


def generate_csharp_artifacts(
    doc: IdlDocument,
    namespace_root: str,
    package_name: str,
    project_name: str,
    idl_header_names: Sequence[str] | None = None,
) -> CSharpArtifacts:
    return CSharpGenerator(
        namespace_root=namespace_root,
        package_name=package_name,
        project_name=project_name,
        idl_header_names=idl_header_names,
    ).generate(doc)
