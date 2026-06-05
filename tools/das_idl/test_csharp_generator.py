import sys
import subprocess
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from csharp_generator import generate_csharp_artifacts
from das_idl_parser import parse_idl, parse_idl_file


ROOT = Path(__file__).resolve().parents[2]


def _contract_doc():
    return parse_idl(
        """
        errorcode DasResult {
            DAS_S_OK = 0,
            DAS_E_INVALID_ARGUMENT = -1073750038,
            DAS_E_FAIL = -2147483648,
        }

        enum DasMode {
            DAS_MODE_FAST = 1,
            DAS_MODE_SAFE = 2,
        }

        struct DasSize {
            uint32_t width;
            uint32_t height;
        }

        namespace Das {
            [uuid("11111111-2222-3333-4444-555555555555")]
            interface IDasSample : IDasBase {
                DasResult GetName([out] IDasReadOnlyString** pp_out_name);
                DasResult SetName(IDasReadOnlyString* p_name);
                DasResult GetSize([out] DasSize* p_out_size);
                DasResult GetMode([out] DasMode* p_out_mode);
                DasResult GetChild([out] IDasSample** pp_out_child);
                DasResult Attach(IDasBase* p_base);
                DasResult Flush();
            }
        }
        """
    )


def _artifacts():
    return generate_csharp_artifacts(
        _contract_doc(),
        namespace_root="Das.Generated",
        package_name="Das.Generated",
        project_name="DasGenerated",
        idl_header_names=["Core.h", "DasResult.h"],
    )


def _source_das_result_artifacts():
    return generate_csharp_artifacts(
        parse_idl_file(str(ROOT / "idl" / "DasResult.idl")),
        namespace_root="Das.Generated",
        package_name="Das.Generated",
        project_name="DasGenerated",
        idl_header_names=["DasResult.idl"],
    )


def _combined_text(artifacts):
    return "\n".join(artifacts.files.values())


def _phase77_contract_doc():
    return parse_idl(
        """
        errorcode DasResult {
            DAS_S_OK = 0,
            DAS_E_INVALID_POINTER = -1073750017,
            DAS_E_CSHARP_ERROR = -1073750021,
            DAS_E_INVALID_ARGUMENT = -1073750038,
            DAS_E_CSHARP_MISSING_RUNTIMECONFIG = -1073750044,
            DAS_E_CSHARP_BOOTSTRAP_INVALID = -1073800002,
            DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED = -1073800003,
        }

        enum DasSampleKind {
            DAS_SAMPLE_TEXT = 1,
            DAS_SAMPLE_BINARY = 2,
        }

        struct DasSampleSize {
            uint32_t width;
            uint32_t height;
        }

        namespace Das {
            [uuid("00000000-0000-0000-0000-000000000003")]
            interface IDasBinaryBuffer : IDasBase {
                DasResult GetData([out] const uint8_t** pp_out_data, [out] size_t* p_out_size);
            }

            [uuid("00000000-0000-0000-0000-000000000004")]
            interface IDasVariantVector : IDasBase {
                DasResult GetSize([out] uint64_t* p_out_size);
            }

            [uuid("00000000-0000-0000-0000-000000000006")]
            interface IDasTypeInfo : IDasBase {
                DasResult GetGuid([out] DasGuid* p_out_guid);
            }

            [uuid("00000000-0000-0000-0000-000000000005")]
            interface IDasComponent : IDasTypeInfo {
                DasResult Dispatch(IDasReadOnlyString* p_function_name, IDasVariantVector* p_arguments);
                DasResult GetSummary([out] IDasReadOnlyString** pp_out_summary);
                DasResult GetBinary([out] IDasBinaryBuffer** pp_out_buffer);
                DasResult GetChild([out] IDasComponent** pp_out_child);
                DasResult GetSizeAndKind([out] DasSampleSize* p_out_size, [out] DasSampleKind* p_out_kind);
                DasResult Attach(IDasBase* p_base, IDasComponent* p_component);
            }

            [uuid("00000000-0000-0000-0000-000000000007")]
            interface IDasComponentFactory : IDasTypeInfo {
                DasResult CreateInstance(const DasGuid& component_iid, [out] IDasComponent** pp_out_component);
            }
        }
        """
    )


def _phase77_artifacts():
    return generate_csharp_artifacts(
        _phase77_contract_doc(),
        namespace_root="Das.Generated",
        package_name="Das.Generated",
        project_name="DasGenerated",
        idl_header_names=["DasResult.idl", "DasCore.idl"],
    )


def _phase79_director_contract_doc():
    return parse_idl(
        """
        errorcode DasResult {
            DAS_S_OK = 0,
            DAS_E_INVALID_POINTER = -1073750017,
            DAS_E_CSHARP_ERROR = -1073750021,
            DAS_E_INVALID_ARGUMENT = -1073750038,
            DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED = -1073800003,
        }

        enum DasPluginFeature {
            DAS_PLUGIN_FEATURE_COMPONENT_FACTORY = 8,
        }

        enum DasVariantType {
            DAS_VARIANT_TYPE_STRING = 2,
            DAS_VARIANT_TYPE_COMPONENT = 5,
        }

        struct DasGuid {
            uint32_t data1;
        }

        namespace Das {
            [uuid("00000000-0000-0000-0000-000000000001")]
            interface IDasVariantVector : IDasBase {
                DasResult GetString(uint64_t index, [out] IDasReadOnlyString** pp_out_string);
                DasResult GetComponent(uint64_t index, [out] IDasComponent** pp_out_component);
                DasResult PushBackString(IDasReadOnlyString* in_string);
                DasResult PushBackComponent(IDasComponent* in_component);
            }

            [uuid("00000000-0000-0000-0000-000000000002")]
            interface IDasTypeInfo : IDasBase {
                DasResult GetGuid([out] DasGuid* p_out_guid);
            }

            [uuid("15FF0855-E031-4602-829D-040230515C55")]
            interface IDasComponent : IDasTypeInfo {
                DasResult Dispatch(IDasReadOnlyString* p_function_name, IDasVariantVector* p_arguments, [out] IDasVariantVector** pp_out_result);
            }

            [uuid("104C288C-5970-40B9-8E3F-B0B7E4ED509A")]
            interface IDasComponentFactory : IDasTypeInfo {
                DasResult IsSupported(const DasGuid& component_iid);
                DasResult CreateInstance(const DasGuid& component_iid, [out] IDasComponent** pp_out_component);
            }

            [uuid("09EA2A40-6A10-4756-AB2B-41B2FD75AB36")]
            interface IDasPluginPackage : IDasBase {
                DasResult EnumFeature(uint64_t index, [out] DasPluginFeature* p_out_feature);
                DasResult CreateFeatureInterface(uint64_t index, [out] IDasBase** pp_out_interface);
                DasResult CanUnloadNow([out] bool* canUnloadNow);
            }
        }
        """
    )


def _phase79_director_artifacts():
    return generate_csharp_artifacts(
        _phase79_director_contract_doc(),
        namespace_root="Das.Generated",
        package_name="Das.Generated",
        project_name="DasGenerated",
        idl_header_names=[
            "DasResult.h",
            "IDasPluginPackage.h",
            "IDasComponent.h",
            "IDasVariantVector.h",
        ],
    )


class TestCSharpGeneratorContract(unittest.TestCase):
    def test_d77_46_d77_47_outputs_stable_per_type_paths(self):
        artifacts = _artifacts()

        self.assertIsInstance(artifacts.files, dict)
        self.assertIn("DasGenerated.csproj", artifacts.files)
        self.assertIn("Das.Generated/DasResult.cs", artifacts.files)
        self.assertIn("Das.Generated/DasException.cs", artifacts.files)
        self.assertIn("Das.Generated/Abi/DasGuid.cs", artifacts.files)
        self.assertIn("Das.Generated/Abi/NativeMethods.cs", artifacts.files)
        self.assertIn("Das.Generated/Interop/NativeHandle.cs", artifacts.files)
        self.assertIn("Das.Generated/Wrappers/IDasSample.cs", artifacts.files)
        self.assertIn("Das.Generated/Directors/IDasSampleDirector.cs", artifacts.files)
        self.assertIn("Das.Generated/Results/IDasSampleResults.cs", artifacts.files)
        self.assertGreaterEqual(
            len([path for path in artifacts.files if path.endswith(".cs")]),
            8,
            "D-77-47: C# reduce output must not collapse into one monolithic .cs file",
        )

    def test_d77_58_generated_namespaces_use_das_root(self):
        combined = _combined_text(_artifacts())

        self.assertIn("namespace Das.Generated", combined)
        self.assertIn("namespace Das.Generated.Abi", combined)
        self.assertIn("namespace Das.Generated.Wrappers", combined)
        self.assertNotIn("namespace DAS.", combined)
        self.assertNotIn("namespace DuskAutoScript.", combined)

    def test_d77_59_public_abi_uses_intptr_not_nint(self):
        combined = _combined_text(_artifacts())

        self.assertIn("System.IntPtr", combined)
        self.assertIn("IntPtr", combined)
        self.assertNotIn("nint", combined)

    def test_d77_19_d77_21_d77_30_result_and_checked_api_shape(self):
        artifacts = _artifacts()
        combined = _combined_text(artifacts)

        self.assertIn("public enum DasResult : int", artifacts.files["Das.Generated/DasResult.cs"])
        self.assertIn("DAS_E_INVALID_ARGUMENT = -1073750038", combined)
        self.assertIn("public sealed class DasException : System.Exception", combined)
        self.assertIn("public static void OrThrow(this DasResult result", combined)
        self.assertIn("public readonly struct IDasSampleResults", combined)
        self.assertIn("public DasResult Flush()", combined)
        self.assertIn("public void FlushOrThrow()", combined)
        self.assertNotIn("Ez", combined)

    def test_d77_16_d77_30_generated_das_result_exposes_csharp_diagnostics(self):
        artifacts = _source_das_result_artifacts()
        das_result = artifacts.files["Das.Generated/DasResult.cs"]
        combined = _combined_text(artifacts)

        for name in (
            "DAS_E_CSHARP_ERROR = -1073750021",
            "DAS_E_CSHARP_MISSING_RUNTIMECONFIG = -1073750044",
            "DAS_E_CSHARP_UNSUPPORTED_TFM = -1073750045",
            "DAS_E_CSHARP_HOSTFXR_INIT_FAILED = -1073750046",
            "DAS_E_CSHARP_COM_CLR_INIT_FAILED = -1073750047",
            "DAS_E_CSHARP_ENTRYPOINT_MISSING = -1073750048",
            "DAS_E_CSHARP_PLUGIN_INIT_FAILED = -1073750049",
        ):
            with self.subTest(name=name):
                self.assertIn(name, das_result)

        self.assertIn("public sealed class DasException : System.Exception", combined)
        self.assertIn("public DasResult Result { get; }", combined)
        self.assertIn("public static void OrThrow(this DasResult result", combined)
        self.assertNotIn("public static class CSharpErrors", combined)

    def test_d77_22_generated_text_has_no_aot_hostile_constructs(self):
        combined = _combined_text(_artifacts())

        forbidden = (
            "dynamic",
            "Assembly.Load",
            "Reflection.Emit",
            "Type.GetType",
            "GetCustomAttributes",
            "GetMethods(",
            "InvokeMember",
            "CreateDelegate",
            "RuntimeHelpers.RunClassConstructor",
        )
        for pattern in forbidden:
            with self.subTest(pattern=pattern):
                self.assertNotIn(pattern, combined)

    def test_d77_55_generated_compile_shape_avoids_csharp_syntax_traps(self):
        doc = parse_idl(
            """
            errorcode DasResult {
                DAS_S_OK = 0,
                DAS_E_INVALID_ARGUMENT = -1073750038,
            }

            struct DasMatchParams {
                uint32_t count;
            }

            namespace Das {
                [uuid("11111111-2222-3333-4444-555555555555")]
                interface IDasKeywordSample : IDasBase {
                    DasResult Create([out] IDasBase** pp_out_interface);
                    DasResult Configure(DasMatchParams params);
                    DasResult At(uint64_t index, [out] DasGuid* p_out_iid, [out] uint64_t* p_out_int);
                }
            }
            """
        )
        artifacts = generate_csharp_artifacts(
            doc,
            namespace_root="Das.Generated",
            package_name="Das.Generated",
            project_name="DasGenerated",
            idl_header_names=["DasResult.idl"],
        )
        combined = _combined_text(artifacts)

        self.assertIn("IDasBase interfaceValue", combined)
        self.assertIn("DasMatchParams paramsValue", combined)
        self.assertIn("ulong intValue", combined)
        self.assertIn("_ = (_handle, index);", combined)
        self.assertNotIn("_ = _handle, index;", combined)
        self.assertNotIn(" IDasBase interface)", combined)
        self.assertNotIn("DasMatchParams params)", combined)
        self.assertNotIn(" ulong int)", combined)

    def test_d77_19_d77_22_d77_59_combined_source_gate_keeps_explicit_abi(self):
        artifacts = _artifacts()
        combined = _combined_text(artifacts)
        wrapper = artifacts.files["Das.Generated/Wrappers/IDasSample.cs"]

        self.assertIn("public DasResult Flush()", combined)
        self.assertIn("public void FlushOrThrow()", combined)
        self.assertIn("public readonly struct NativeHandle", combined)
        self.assertIn("public System.IntPtr Value", combined)
        self.assertIn("unsafe partial class NativeMethods", combined)
        self.assertIn("delegate* unmanaged<System.IntPtr, int>", combined)
        self.assertLess(wrapper.index("public DasResult Flush()"), wrapper.index("FlushOrThrow"))
        self.assertNotIn("throw new DasException", wrapper)
        self.assertNotIn("public nint", combined)

    def test_d77_23_d77_25_d77_26_modern_interop_has_tfm_gates(self):
        native_methods = _artifacts().files["Das.Generated/Abi/NativeMethods.cs"]

        self.assertIn("#if NET5_0_OR_GREATER", native_methods)
        self.assertIn("#if NET7_0_OR_GREATER", native_methods)
        self.assertIn("#if NET8_0_OR_GREATER", native_methods)
        self.assertIn("#if NETFRAMEWORK", native_methods)
        self.assertIn("LibraryImport", native_methods)
        self.assertIn("delegate* unmanaged", native_methods)


class TestCSharpExportCli(unittest.TestCase):
    def test_csharp_cli_merges_idl_and_writes_artifacts(self):
        script = Path(__file__).parent / "das_csharp_export.py"
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            idl_dir = temp / "idl"
            output_dir = temp / "out"
            idl_dir.mkdir()
            (idl_dir / "Result.idl").write_text(
                "errorcode DasResult { DAS_S_OK = 0, }",
                encoding="utf-8",
            )
            (idl_dir / "Core.idl").write_text(
                """
                namespace Das {
                    [uuid("11111111-2222-3333-4444-555555555555")]
                    interface IDasSample : IDasBase {
                        DasResult Flush();
                    }
                }
                """,
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(script),
                    "--idl-dir",
                    str(idl_dir),
                    "--output",
                    str(output_dir),
                    "--namespace-root",
                    "Das.Generated",
                    "--package-name",
                    "Das.Generated",
                    "--project-name",
                    "DasGenerated",
                    "--idl-files",
                    "Result.idl",
                    "Core.idl",
                ],
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((output_dir / "DasGenerated.csproj").exists())
            self.assertTrue((output_dir / "Das.Generated" / "DasResult.cs").exists())
            self.assertTrue(
                (output_dir / "Das.Generated" / "Wrappers" / "IDasSample.cs").exists()
            )
            self.assertIn(
                "DAS_S_OK",
                (output_dir / "Das.Generated" / "DasResult.cs").read_text(
                    encoding="utf-8"
                ),
            )
            self.assertIn("Generated:", result.stdout)


class TestCSharpGeneratorPhase77CompleteSurface(unittest.TestCase):
    def test_d77_21_d77_24_full_type_mapping_supports_core_abi_shapes(self):
        artifacts = _phase77_artifacts()
        combined = _combined_text(artifacts)

        self.assertIn("public enum DasSampleKind : int", combined)
        self.assertIn("public readonly struct DasSampleSize", combined)
        self.assertIn("public readonly struct DasGuid", combined)
        self.assertIn("DAS_E_CSHARP_ERROR = -1073750021", combined)
        self.assertIn("DAS_E_CSHARP_MISSING_RUNTIMECONFIG = -1073750044", combined)
        self.assertIn("public sealed class DasReadOnlyString", combined)
        self.assertIn("public sealed class DasBinaryBuffer", combined)
        self.assertIn("public sealed class IDasComponent", combined)
        self.assertIn("public IDasComponentGetSizeAndKindResult GetSizeAndKind()", combined)
        self.assertIn("public IDasComponentGetSizeAndKindResult GetSizeAndKindOrThrow()", combined)

    def test_d77_28_d77_29_d77_33_string_input_has_object_and_utf16_overloads(self):
        wrapper = _phase77_artifacts().files["Das.Generated/Wrappers/IDasComponent.cs"]

        self.assertIn(
            "public DasResult Dispatch(DasReadOnlyString functionName, IDasVariantVector arguments)",
            wrapper,
        )
        self.assertIn(
            "public unsafe DasResult Dispatch(string functionName, IDasVariantVector arguments)",
            wrapper,
        )
        self.assertIn("ArgumentNullException.ThrowIfNull(functionName);", wrapper)
        self.assertIn("fixed (char* pValue = functionName)", wrapper)
        self.assertIn("(ushort*)pValue", wrapper)
        self.assertIn("checked((nuint)functionName.Length)", wrapper)
        self.assertIn("CreateIDasReadOnlyStringFromUtf16WithLength", wrapper)
        self.assertIn("using var functionNameString", wrapper)

    def test_d77_34_d77_38_d77_44_d77_48_string_helper_preserves_code_units(self):
        artifacts = _phase77_artifacts()
        string_interop = artifacts.files[
            "Das.Generated/Interop/DasStringInterop.cs"
        ]
        read_only_string = artifacts.files["Das.Generated/Wrappers/DasReadOnlyString.cs"]
        combined = _combined_text(artifacts)

        self.assertIn("ArgumentNullException.ThrowIfNull(value);", string_interop)
        self.assertIn("internal const string EmbeddedNul = \"left\\0right\";", string_interop)
        self.assertIn("internal const string UnpairedSurrogate = \"\\ud800\";", string_interop)
        self.assertIn("GetIDasReadOnlyStringUtf16", read_only_string)
        self.assertIn("CopyUtf16(pUtf16, length)", read_only_string)
        self.assertIn("new string((char*)pUtf16, 0, checked((int)length))", string_interop)
        self.assertNotIn("Rune", combined)
        self.assertNotIn("IsSurrogatePair", combined)
        self.assertNotIn("Normalize(", combined)
        self.assertNotIn("string.Empty", combined)

    def test_d77_24_d77_30_out_results_and_interface_returns_are_typed(self):
        artifacts = _phase77_artifacts()
        results = artifacts.files["Das.Generated/Results/IDasComponentResults.cs"]
        wrapper = artifacts.files["Das.Generated/Wrappers/IDasComponent.cs"]

        self.assertIn("public readonly struct IDasComponentGetSummaryResult", results)
        self.assertIn("public DasReadOnlyString Summary { get; }", results)
        self.assertIn("public readonly struct IDasComponentGetBinaryResult", results)
        self.assertIn("public DasBinaryBuffer Buffer { get; }", results)
        self.assertIn("public readonly struct IDasComponentGetChildResult", results)
        self.assertIn("public IDasComponent Child { get; }", results)
        self.assertIn("public DasSampleSize Size { get; }", results)
        self.assertIn("public DasSampleKind Kind { get; }", results)
        self.assertIn("public IDasComponentGetChildResult GetChildOrThrow()", wrapper)
        self.assertIn("result.Result.OrThrow();", wrapper)

    def test_d77_24_interface_inputs_use_same_type_and_upcast_wrappers_only(self):
        wrapper = _phase77_artifacts().files["Das.Generated/Wrappers/IDasComponent.cs"]

        self.assertIn("public DasResult Attach(IDasBase baseObject, IDasComponent component)", wrapper)
        self.assertIn("baseObject.Handle", wrapper)
        self.assertIn("component.Handle", wrapper)
        self.assertIn("CanAssignTo(\"IDasBase\")", wrapper)
        self.assertIn("CanAssignTo(\"IDasComponent\")", wrapper)
        self.assertNotIn("QueryInterface(", wrapper)
        self.assertNotIn("Downcast", wrapper)
        self.assertNotIn("Sidecast", wrapper)

    def test_d77_24_validation_failure_for_out_result_returns_typed_result(self):
        doc = parse_idl(
            """
            errorcode DasResult {
                DAS_S_OK = 0,
                DAS_E_INVALID_ARGUMENT = -1073750038,
            }

            namespace Das {
                [uuid("11111111-2222-3333-4444-555555555555")]
                interface IDasValidationSample : IDasBase {
                    DasResult Find(IDasBase* p_base, [out] IDasBase** pp_out_child);
                }
            }
            """
        )
        artifacts = generate_csharp_artifacts(
            doc,
            namespace_root="Das.Generated",
            package_name="Das.Generated",
            project_name="DasGenerated",
            idl_header_names=["DasResult.idl"],
        )
        wrapper = artifacts.files["Das.Generated/Wrappers/IDasValidationSample.cs"]

        self.assertNotIn(
            "return DasResult.DAS_E_INVALID_ARGUMENT;",
            wrapper,
        )
        self.assertIn(
            "public IDasValidationSampleFindResult Find(IDasBase baseObject)",
            wrapper,
        )
        self.assertIn(
            "return new IDasValidationSampleFindResult(DasResult.DAS_E_INVALID_ARGUMENT, "
            "new IDasBase(System.IntPtr.Zero));",
            wrapper,
        )

    def test_d77_24_binary_buffer_return_has_explicit_view_shape(self):
        artifacts = _phase77_artifacts()
        binary = artifacts.files["Das.Generated/Wrappers/DasBinaryBuffer.cs"]
        combined = _combined_text(artifacts)

        self.assertIn("public sealed class DasBinaryBuffer : IDisposable", binary)
        self.assertIn("public System.IntPtr Handle", binary)
        self.assertIn("public DasResult GetView(out DasBinaryBufferView view)", binary)
        self.assertIn("public readonly struct DasBinaryBufferView", binary)
        self.assertNotIn("dynamic", combined)
        self.assertNotIn("Reflection.Emit", combined)

    def test_d77_27_director_uses_normal_gchandle_and_release_once(self):
        director = _phase77_artifacts().files[
            "Das.Generated/Directors/IDasComponentDirector.cs"
        ]

        self.assertIn("GCHandle.Alloc(callbacks, GCHandleType.Normal)", director)
        self.assertNotIn("GCHandleType.Pinned", director)
        self.assertIn("public System.IntPtr ManagedState => GCHandle.ToIntPtr", director)
        self.assertIn("internal static void ReleaseManagedState(System.IntPtr managedState)", director)
        self.assertEqual(director.count("handle.Free();"), 1)
        self.assertIn("delegate* unmanaged<System.IntPtr", director)
        self.assertIn("managed_state", director)

    def test_d77_43_native_director_support_files_and_boundary_checks_exist(self):
        artifacts = _phase77_artifacts()
        self.assertIn("Native/DasCSharpDirectorSupport.h", artifacts.files)
        self.assertIn("Native/DasCSharpDirectorSupport.cpp", artifacts.files)

        header = artifacts.files["Native/DasCSharpDirectorSupport.h"]
        source = artifacts.files["Native/DasCSharpDirectorSupport.cpp"]

        self.assertIn("DasCreateCSharpIDasComponentDirector", header)
        self.assertIn("IDasComponentDirectorCallbacks", header)
        self.assertLess(source.index("if (pp_out_object == nullptr)"), source.index("new CSharpIDasComponentDirector"))
        self.assertLess(source.index("if (callbacks == nullptr)"), source.index("new CSharpIDasComponentDirector"))
        self.assertLess(source.index("if (managed_state == 0)"), source.index("new CSharpIDasComponentDirector"))
        self.assertIn("return DAS_E_INVALID_POINTER;", source)
        self.assertIn("return DAS_E_INVALID_ARGUMENT;", source)
        self.assertIn("*pp_out_object = nullptr;", source)

    def test_d77_43_native_factory_failures_do_not_return_objects(self):
        source = _phase77_artifacts().files["Native/DasCSharpDirectorSupport.cpp"]

        invalid_pointer_blocks = (
            "if (pp_out_object == nullptr)\n"
            "    {\n"
            "        return DAS_E_INVALID_POINTER;\n"
            "    }",
            "if (callbacks == nullptr)\n"
            "    {\n"
            "        *pp_out_object = nullptr;\n"
            "        return DAS_E_INVALID_POINTER;\n"
            "    }",
        )
        for block in invalid_pointer_blocks:
            with self.subTest(block=block):
                self.assertIn(block, source)
        self.assertIn(
            "if (managed_state == 0)\n"
            "    {\n"
            "        *pp_out_object = nullptr;\n"
            "        return DAS_E_INVALID_ARGUMENT;\n"
            "    }",
            source,
        )

    def test_d78_05_bootstrap_helper_has_dual_entrypoint_signatures(self):
        artifacts = _phase77_artifacts()
        self.assertIn(
            "Das.Generated/Runtime/DasCSharpBootstrap.cs",
            artifacts.files,
        )
        bootstrap = artifacts.files["Das.Generated/Runtime/DasCSharpBootstrap.cs"]

        self.assertIn(
            "public static DasResult Invoke(\n"
            "        IntPtr args,\n"
            "        int sizeBytes,\n"
            "        Func<object> packageFactory)",
            bootstrap,
        )
        self.assertIn(
            "public static DasResult Invoke(\n"
            "        string bootstrapCookie,\n"
            "        Func<object> packageFactory)",
            bootstrap,
        )
        self.assertIn("private static IntPtr DecodeBootstrapCookie", bootstrap)
        self.assertIn("ValidateBootstrapArgs(bootstrapArgs, sizeBytes)", bootstrap)
        self.assertIn("args->size != Marshal.SizeOf<DasCSharpBootstrapArgsV1>()", bootstrap)
        self.assertIn(
            "args->abi_version != DAS_CSHARP_BOOTSTRAP_ARGS_V1_ABI_VERSION",
            bootstrap,
        )
        self.assertIn("Marshal.WriteIntPtr(packageOut, IntPtr.Zero);", bootstrap)
        self.assertIn("catch (Exception)", bootstrap)
        self.assertIn("return DasResult.DAS_E_CSHARP_BOOTSTRAP_INVALID;", bootstrap)
        self.assertNotIn("DasResult.DAS_CSHARP_BOOTSTRAP_INVALID", bootstrap)
        self.assertNotIn("public nint", bootstrap)

    def test_d78_05_csharp_support_retains_managed_state_without_pinning(self):
        artifacts = _phase77_artifacts()
        director = artifacts.files["Das.Generated/Directors/IDasComponentDirector.cs"]
        combined = _combined_text(artifacts)

        self.assertIn("GCHandle.Alloc(callbacks, GCHandleType.Normal)", director)
        self.assertNotIn("GCHandleType.Pinned", combined)
        self.assertIn(
            "internal static void ReleaseManagedState(System.IntPtr managedState)",
            director,
        )
        self.assertEqual(director.count("handle.Free();"), 1)

    def test_d78_05_native_director_support_has_com_lifetime_and_qi(self):
        artifacts = _phase77_artifacts()
        header = artifacts.files["Native/DasCSharpDirectorSupport.h"]
        source = artifacts.files["Native/DasCSharpDirectorSupport.cpp"]

        self.assertIn("using DasCSharpDirectorReleaseThunk", header)
        self.assertIn("DasCSharpDirectorReleaseThunk release;", header)
        self.assertIn("DasResult (*Dispatch)", header)
        self.assertIn("DasResult (*CreateInstance)", header)
        self.assertIn("uint32_t DAS_STD_CALL AddRef() override", source)
        self.assertIn("uint32_t DAS_STD_CALL Release() override", source)
        self.assertIn("void FinalRelease() noexcept", source)
        self.assertEqual(source.count("release_(managed_state_);"), 1)
        self.assertIn("DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override", source)
        self.assertIn("if (iid == DAS_IID_BASE)", source)
        self.assertIn("if (iid == DasIidOf<IDasComponent>())", source)
        self.assertIn("static_cast<IDasComponent*>(this)", source)
        self.assertIn("static_cast<IDasTypeInfo*>(this)", source)
        self.assertIn("AddRef();", source)
        self.assertIn("return DAS_E_NO_INTERFACE;", source)

    def test_d78_05_native_callback_table_forwards_methods_and_nulls_out_params(self):
        source = _phase77_artifacts().files["Native/DasCSharpDirectorSupport.cpp"]

        self.assertIn("callbacks_->Dispatch(", source)
        self.assertIn("callbacks_->CreateInstance(", source)
        self.assertIn("if (pp_out_object == nullptr)", source)
        self.assertIn("*pp_out_object = nullptr;", source)
        self.assertIn("if (callbacks->release == nullptr)", source)
        self.assertIn("return DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED;", source)
        self.assertLess(
            source.index("if (callbacks->release == nullptr)"),
            source.index("new (std::nothrow) CSharpIDasComponentDirector"),
        )
        self.assertIn("if (object == nullptr)", source)
        self.assertIn("return DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED;", source)

    def test_d77_47_reviewable_layout_and_forbidden_surface_scan(self):
        artifacts = _phase77_artifacts()
        combined = _combined_text(artifacts)

        self.assertGreaterEqual(len([path for path in artifacts.files if path.endswith(".cs")]), 20)
        for path in (
            "Das.Generated/Wrappers/IDasBase.cs",
            "Das.Generated/Wrappers/IDasReadOnlyString.cs",
            "Das.Generated/Wrappers/DasReadOnlyString.cs",
            "Das.Generated/Wrappers/DasBinaryBuffer.cs",
            "Das.Generated/Directors/IDasComponentDirector.cs",
            "Das.Generated/Results/IDasComponentResults.cs",
            "Native/DasCSharpDirectorSupport.h",
            "Native/DasCSharpDirectorSupport.cpp",
        ):
            with self.subTest(path=path):
                self.assertIn(path, artifacts.files)

        for token in (
            "Ez",
            "SWIG",
            "CSharpSwig",
            "dynamic",
            "Assembly.Load",
            "Reflection.Emit",
            "runtimeKind",
            "DuskAutoScript.",
        ):
            with self.subTest(token=token):
                self.assertNotIn(token, combined)

    def test_d77_47_output_count_scales_with_type_graph(self):
        simple_artifacts = _artifacts()
        rich_artifacts = _phase77_artifacts()

        simple_csharp_paths = [path for path in simple_artifacts.files if path.endswith(".cs")]
        rich_csharp_paths = [path for path in rich_artifacts.files if path.endswith(".cs")]

        self.assertGreater(len(rich_csharp_paths), len(simple_csharp_paths))
        self.assertIn("Das.Generated/Wrappers/IDasBase.cs", rich_artifacts.files)
        self.assertIn("Das.Generated/Wrappers/IDasReadOnlyString.cs", rich_artifacts.files)
        self.assertIn("Native/DasCSharpDirectorSupport.h", rich_artifacts.files)
        self.assertIn("Native/DasCSharpDirectorSupport.cpp", rich_artifacts.files)


class TestCSharpGeneratorPhase79NativeDirectorSurface(unittest.TestCase):
    def test_d79_01_d79_02_director_create_apis_return_wrappers(self):
        artifacts = _phase79_director_artifacts()

        expected = {
            "IDasPluginPackage": "DasCreateCSharpIDasPluginPackageDirector",
            "IDasComponentFactory": "DasCreateCSharpIDasComponentFactoryDirector",
            "IDasComponent": "DasCreateCSharpIDasComponentDirector",
        }
        for interface_name, factory_name in expected.items():
            with self.subTest(interface_name=interface_name):
                director = artifacts.files[
                    f"Das.Generated/Directors/{interface_name}Director.cs"
                ]
                self.assertIn(
                    f"public static Das.Generated.Wrappers.{interface_name} Create(",
                    director,
                    f"D-79-01: {interface_name}Director must expose a Create API "
                    "that returns a generated wrapper with a real native handle.",
                )
                self.assertIn(
                    f"NativeMethods.{factory_name}(",
                    director,
                    f"D-79-02: {interface_name}Director.Create must call the stable "
                    "native director factory.",
                )
                self.assertIn(
                    f"return new Das.Generated.Wrappers.{interface_name}(nativeHandle);",
                    director,
                    f"D-79-01: {interface_name}Director.Create must wrap the "
                    "nonzero native handle returned by the support DLL.",
                )

    def test_d79_01_d79_03_director_callbacks_keep_out_param_shapes(self):
        artifacts = _phase79_director_artifacts()
        package_director = artifacts.files[
            "Das.Generated/Directors/IDasPluginPackageDirector.cs"
        ]
        factory_director = artifacts.files[
            "Das.Generated/Directors/IDasComponentFactoryDirector.cs"
        ]
        component_director = artifacts.files[
            "Das.Generated/Directors/IDasComponentDirector.cs"
        ]

        self.assertIn(
            "DasResult CreateFeatureInterface(ulong index, out System.IntPtr interfaceHandle);",
            package_director,
            "D-79-01: plugin package callbacks must carry the native out interface handle.",
        )
        self.assertIn(
            "DasResult CreateInstance(DasGuid componentIid, out System.IntPtr componentHandle);",
            factory_director,
            "D-79-01: component factory callbacks must carry the native out component handle.",
        )
        self.assertIn(
            "DasResult Dispatch(DasReadOnlyString functionName, IDasVariantVector arguments, out System.IntPtr resultHandle);",
            component_director,
            "D-79-03: component Dispatch callback must return DasResult and an out vector handle.",
        )
        for director in (package_director, factory_director, component_director):
            self.assertIn("catch (Exception)", director)
            self.assertIn("return DasResult.DAS_E_CSHARP_ERROR;", director)

    def test_d79_02_native_methods_split_director_support_from_core_das(self):
        native_methods = _phase79_director_artifacts().files[
            "Das.Generated/Abi/NativeMethods.cs"
        ]

        self.assertIn(
            'internal const string DAS_CSHARP_NATIVE_SUPPORT_MODULE = "DasCSharpNativeSupport";',
            native_methods,
            "D-79-02: director factories must bind to the stable support DLL name.",
        )
        for factory_name in (
            "DasCreateCSharpIDasPluginPackageDirector",
            "DasCreateCSharpIDasComponentFactoryDirector",
            "DasCreateCSharpIDasComponentDirector",
        ):
            with self.subTest(factory_name=factory_name):
                self.assertIn(factory_name, native_methods)
                self.assertIn(
                    "LibraryImport(DAS_CSHARP_NATIVE_SUPPORT_MODULE",
                    native_methods,
                    "D-79-02: director factory imports must not bind to das.",
                )

        for core_api in (
            'LibraryImport("das", EntryPoint = "DasAddRef")',
            'LibraryImport("das", EntryPoint = "CreateIDasVariantVector")',
            'LibraryImport("das", EntryPoint = "GetIDasVariantVectorString")',
            'LibraryImport("das", EntryPoint = "PushBackIDasVariantVectorComponent")',
        ):
            with self.subTest(core_api=core_api):
                self.assertIn(core_api, native_methods)

    def test_d79_04_d79_06_variant_vector_wrapper_uses_native_calls(self):
        artifacts = _phase79_director_artifacts()
        wrapper = artifacts.files["Das.Generated/Wrappers/IDasVariantVector.cs"]
        native_methods = artifacts.files["Das.Generated/Abi/NativeMethods.cs"]

        expected_calls = (
            "NativeMethods.GetIDasVariantVectorString(",
            "NativeMethods.GetIDasVariantVectorComponent(",
            "NativeMethods.PushBackIDasVariantVectorString(",
            "NativeMethods.PushBackIDasVariantVectorComponent(",
            "NativeMethods.CreateIDasVariantVector(",
        )
        for call in expected_calls:
            with self.subTest(call=call):
                self.assertIn(call, wrapper + native_methods)

        self.assertNotIn("_ = (_handle, index);", wrapper)
        self.assertNotIn("_ = (_handle, inString);", wrapper)
        self.assertNotIn("_ = (_handle, inComponent);", wrapper)

    def test_d79_08_d79_09_release_thunk_runs_hook_then_frees_once(self):
        director = _phase79_director_artifacts().files[
            "Das.Generated/Directors/IDasComponentDirector.cs"
        ]
        combined = _combined_text(_phase79_director_artifacts())

        self.assertIn("GCHandle.Alloc(state, GCHandleType.Normal)", director)
        self.assertNotIn("GCHandleType.Pinned", combined)
        self.assertIn("state.OnFinalRelease();", director)
        self.assertLess(
            director.index("state.OnFinalRelease();"),
            director.index("handle.Free();"),
            "D-79-09: managed final-release hook must run before freeing the GCHandle.",
        )
        self.assertEqual(director.count("handle.Free();"), 1)
        for forbidden in ("dynamic", "GetCustomAttributes", "SWIG"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, combined)


if __name__ == "__main__":
    unittest.main()
