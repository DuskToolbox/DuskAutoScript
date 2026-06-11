import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _iter_source_files(*roots: str):
    suffixes = {
        ".c",
        ".cc",
        ".cpp",
        ".cxx",
        ".h",
        ".hpp",
        ".cs",
        ".cmake",
        ".json",
        ".in",
        ".py",
        ".txt",
    }
    ignored_dirs = {
        ".git",
        ".planning",
        ".vs",
        ".vscode",
        "build",
        "cmake-build-debug",
        "cmake-build-release",
        "__pycache__",
        "bin",
        "obj",
        "logs",
    }
    for root_name in roots:
        root = ROOT / root_name
        if root.is_file():
            yield root
            continue
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            parts = set(path.relative_to(ROOT).parts)
            if parts & ignored_dirs:
                continue
            if path.suffix.lower() in suffixes:
                yield path


def _rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


class TestCSharpHostBridgeBoundaries(unittest.TestCase):
    def test_public_headers_no_csharp_helper_abi_leak(self):
        public_headers = "\n".join(
            (
                _read_text(
                    ROOT / "include" / "das" / "DasApi.h"
                ),
                _read_text(
                    ROOT / "include" / "das" / "DasString.hpp"
                ),
            )
        )

        for helper_name in (
            "DasAddRef",
            "DasRelease",
            "GetIDasVariantVectorString",
            "GetIDasVariantVectorComponent",
            "PushBackIDasVariantVectorString",
            "PushBackIDasVariantVectorComponent",
            "DispatchIDasComponent",
            "GetIDasReadOnlyStringUtf16",
        ):
            with self.subTest(helper_name=helper_name):
                self.assertNotIn(
                    helper_name,
                    public_headers,
                    "Helper ABI belongs to generated C# native support runtime and must remain out of public headers.",
                )

    def test_csharp_swig_route_is_not_restored(self):
        self.assertFalse(
            (ROOT / "tools" / "das_idl" / "swig_csharp_generator.py").exists()
        )

        self.assertFalse(
            (ROOT / "SWIG" / "csharp").exists(),
            "SWIG/csharp/ must not exist — C# SWIG .i files have been removed",
        )

        forbidden = {
            "CSharpSwigGenerator": [
                "tools/das_idl/test_csharp_batch_config.py",
                "tools/das_idl/test_csharp_host_bridge_boundaries.py",
            ],
            "SWIGCSHARP": [
                "tools/das_idl/swig_lang_generator_base.py",
                "tools/das_idl/swig_java_generator.py",
                "tools/das_idl/test_csharp_batch_config.py",
                "tools/das_idl/test_csharp_host_bridge_boundaries.py",
            ],
            "das_check_language_export(CSHARP": [
                "tools/das_idl/test_csharp_host_bridge_boundaries.py",
            ],
            'set(_LANG_NAME "CSharp")': [
                "tools/das_idl/test_csharp_batch_config.py",
                "tools/das_idl/test_csharp_host_bridge_boundaries.py",
            ],
        }
        violations = []
        for path in _iter_source_files("3rdParty", "cmake", "tools/das_idl"):
            text = _read_text(path)
            rel = _rel(path)
            for token, allowed_paths in forbidden.items():
                if token in text and rel not in allowed_paths:
                    violations.append(f"{rel}: {token}")

        self.assertEqual(violations, [])

    def test_csharp_manifest_contract_stays_minimal(self):
        manifest_paths = [
            ROOT
            / "das"
            / "Plugins"
            / "CSharpTestPlugin"
            / "CSharpTestPlugin.net8.json.in",
            ROOT
            / "das"
            / "Plugins"
            / "CSharpTestPlugin"
            / "CSharpTestPlugin.net48.json.in",
        ]
        forbidden_patterns = {
            "runtimeKind": r'"runtimeKind"',
            "CSharpDotNet": r"\bCSharpDotNet\b",
            "CSharpDotNetFramework": r"\bCSharpDotNetFramework\b",
            "assemblyPath": r'"assemblyPath"',
            "dependencies": r'"dependencies"',
            "runtimeRequirements": r'"runtimeRequirements"',
        }

        for path in manifest_paths:
            with self.subTest(path=_rel(path)):
                self.assertTrue(path.exists())
                text = _read_text(path)
                for token in (
                    '"name"',
                    '"guid"',
                    '"language"',
                    '"CSharp"',
                    '"pluginFilenameExtension"',
                    '"dll"',
                    '"targetFramework"',
                    '"entryPoint"',
                ):
                    self.assertIn(token, text)
                for token, pattern in forbidden_patterns.items():
                    self.assertIsNone(re.search(pattern, text), token)

        modern = _read_text(manifest_paths[0])
        net48 = _read_text(manifest_paths[1])
        self.assertIn('"runtimeConfigPath"', modern)
        self.assertNotIn('"runtimeConfigPath"', net48)
        self.assertNotEqual(
            re.search(r'"name"\s*:\s*"([^"]+)"', modern).group(1),
            re.search(r'"name"\s*:\s*"([^"]+)"', net48).group(1),
        )
        self.assertNotEqual(
            re.search(r'"guid"\s*:\s*"([^"]+)"', modern).group(1),
            re.search(r'"guid"\s*:\s*"([^"]+)"', net48).group(1),
        )

    def test_public_csharp_runtime_contracts_do_not_expose_old_fields(self):
        forbidden_patterns = {
            "runtimeKind": r'"runtimeKind"',
            "CSharpDotNet": r"\bCSharpDotNet\b",
            "CSharpDotNetFramework": r"\bCSharpDotNetFramework\b",
            "assemblyPath": r'"assemblyPath"',
            "dependencies": r'"dependencies"',
            "runtimeRequirements": r'"runtimeRequirements"',
        }
        allowed_paths = {
            "tools/das_idl/test_csharp_generator.py",
            "tools/das_idl/test_csharp_host_bridge_boundaries.py",
        }
        violations = []
        for path in _iter_source_files(
            "das/Core/ForeignInterfaceHost",
            "das/Plugins/CSharpTestPlugin",
            "tools/das_idl",
        ):
            rel = _rel(path)
            if rel in allowed_paths:
                continue
            text = _read_text(path)
            for token, pattern in forbidden_patterns.items():
                if re.search(pattern, text):
                    violations.append(f"{rel}: {token}")

        self.assertEqual(violations, [])

    def test_csharp_manifest_fields_do_not_enter_ipc_payloads(self):
        forbidden_fields = {
            "targetFramework",
            "entryPoint",
            "runtimeConfigPath",
        }
        allowed_paths = {
            "das/Core/ForeignInterfaceHost/include/das/Core/ForeignInterfaceHost/CSharpBootstrap.h",
            "das/Core/ForeignInterfaceHost/include/das/Core/ForeignInterfaceHost/CSharpManifest.h",
            "das/Core/ForeignInterfaceHost/src/CSharpHost.cpp",
            "das/Core/ForeignInterfaceHost/src/CSharpManifest.cpp",
            "das/Core/ForeignInterfaceHost/test/CSharpHostManifestTest.cpp",
            "das/Core/ForeignInterfaceHost/test/CSharpHostRoutingTest.cpp",
            "das/Core/ForeignInterfaceHost/test/PluginManagerTest.cpp",
            "das/Plugins/CSharpTestPlugin/CSharpTestPlugin.json",
            "das/Plugins/CSharpTestPlugin/CSharpTestPlugin.net8.json.in",
            "das/Plugins/CSharpTestPlugin/CSharpTestPlugin.net48.json.in",
            "tools/das_idl/napi_generator.py",
            "tools/das_idl/test_napi_generator.py",
            "tools/das_idl/test_napi_batch_config.py",
            "tools/das_idl/test_csharp_host_bridge_boundaries.py",
        }
        violations = []
        for path in _iter_source_files(
            "das/Core/IPC",
            "das/Core/ForeignInterfaceHost/include/das/Core/ForeignInterfaceHost/RuntimeProvider.h",
            "das/Core/ForeignInterfaceHost/include/das/Core/ForeignInterfaceHost/RemotePluginHost.h",
            "das/Core/ForeignInterfaceHost/src/RuntimeProvider.cpp",
            "das/Core/ForeignInterfaceHost/src/NativeIpcRuntime.cpp",
            "das/Core/ForeignInterfaceHost/src/RemotePluginHost.cpp",
            "das/Host",
        ):
            rel = _rel(path)
            if rel in allowed_paths:
                continue
            text = _read_text(path)
            for field in forbidden_fields:
                if field in text:
                    violations.append(f"{rel}: {field}")

        self.assertEqual(violations, [])

    def test_host_bridge_does_not_touch_runtime_e2e_or_remote_boundaries(self):
        diff_targets = {
            "das/IpcMultiProcessTest": "C# runtime proof belongs to Phase 79",
            "das/Core/IPC": "remote package addressing belongs to Phase 80",
            "das/Host/src/main.cpp": "direct NativeAOT CSharpHost loading is out of scope",
        }
        # This plan may statically inspect these directories, but it must not
        # add or modify them.
        import subprocess

        violations = []
        for target, reason in diff_targets.items():
            result = subprocess.run(
                ["git", "diff", "--name-only", "--", target],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            changed = [line for line in result.stdout.splitlines() if line.strip()]
            if changed:
                violations.append(f"{target}: {reason}: {changed}")

        self.assertEqual(violations, [])

    def test_csharp_source_and_headers_are_macro_guarded(self):
        guarded_files = [
            "das/Core/ForeignInterfaceHost/src/CSharpHost.h",
            "das/Core/ForeignInterfaceHost/src/CSharpHost.cpp",
            "das/Core/ForeignInterfaceHost/src/CSharpHostFxrBackend.h",
            "das/Core/ForeignInterfaceHost/src/CSharpHostFxrBackend.cpp",
            "das/Core/ForeignInterfaceHost/src/CSharpNetFxBackend.h",
            "das/Core/ForeignInterfaceHost/src/CSharpNetFxBackend.cpp",
        ]

        for rel_path in guarded_files:
            path = ROOT / rel_path
            with self.subTest(path=rel_path):
                text = _read_text(path)
                self.assertIn("DAS_EXPORT_CSHARP", text)
                self.assertIn("#ifdef DAS_EXPORT_CSHARP", text)
                self.assertTrue(
                    re.search(r"^#endif\s*//\s*DAS_EXPORT_CSHARP", text, re.M)
                    is not None,
                    "missing closing DAS_EXPORT_CSHARP guard",
                )

    def test_csharp_factory_branch_only_under_export_guard(self):
        runtime_cpp = ROOT / "das" / "Core" / "ForeignInterfaceHost" / "src" / "IForeignLanguageRuntime.cpp"
        text = _read_text(runtime_cpp)

        match = re.search(r"case CSharp:(.*?)case Java:", text, re.S)
        self.assertIsNotNone(match)
        csharp_block = match.group(1)
        self.assertIn("#ifndef DAS_EXPORT_CSHARP", csharp_block)
        self.assertIn("#else", csharp_block)
        self.assertIn("#endif // DAS_EXPORT_CSHARP", csharp_block)
        self.assertIn(
            "return CSharpHost::CreateForeignLanguageRuntime(desc_base);",
            csharp_block,
        )

    def test_export_csharp_stays_out_of_swig_needed_routing(self):
        self.assertFalse(
            (ROOT / "tools" / "das_idl" / "swig_csharp_generator.py").exists(),
            "swig_csharp_generator.py must not exist per D-79-04",
        )

        gen_batch_config_text = _read_text(
            ROOT / "tools" / "das_idl" / "gen_batch_config.py"
        )
        self.assertIn(
            'swig_languages = {"python", "java"}',
            gen_batch_config_text,
            "swig_languages must be exactly {python, java}; CSharp is excluded per D-79-05",
        )

        cmake_text = _read_text(ROOT / "cmake" / "DasAddIdlExport.cmake")
        need_swig_match = re.search(
            r"set\(_NEED_SWIG FALSE\)(.*?)endif\(\)",
            cmake_text,
            re.S,
        )
        self.assertIsNotNone(need_swig_match, "_NEED_SWIG block not found")
        need_swig_block = need_swig_match.group(1)
        self.assertNotIn(
            "csharp",
            need_swig_block.lower(),
            "_NEED_SWIG must not include csharp per D-79-05",
        )

        csharp_continue_match = re.search(
            r'_LANG_LOWER STREQUAL "csharp".*?continue\(\)',
            cmake_text,
            re.S | re.I,
        )
        self.assertIsNotNone(
            csharp_continue_match,
            "CSharp must continue() past das_add_swig_export_library per D-79-06",
        )

        forbidden = {
            "CSharpSwigGenerator": [
                "tools/das_idl/test_csharp_batch_config.py",
                "tools/das_idl/test_csharp_host_bridge_boundaries.py",
            ],
            "SWIGCSHARP": [
                "tools/das_idl/swig_lang_generator_base.py",
                "tools/das_idl/swig_java_generator.py",
                "tools/das_idl/test_csharp_batch_config.py",
                "tools/das_idl/test_csharp_host_bridge_boundaries.py",
            ],
        }
        violations = []
        for path in _iter_source_files("cmake", "tools/das_idl"):
            text = _read_text(path)
            rel = _rel(path)
            for token, allowed_paths in forbidden.items():
                if token in text and rel not in allowed_paths:
                    violations.append(f"{rel}: {token}")

        self.assertEqual(
            violations,
            [],
            "C# must not participate in SWIG routing (D-79-04, D-79-05)",
        )

    def test_csharp_tokens_are_only_in_pure_generator_host_plugin_or_tests(self):
        allowed_csharp_paths = {
            "tools/das_idl/csharp_generator.py",
            "tools/das_idl/das_csharp_export.py",
            "tools/das_idl/gen_batch_config.py",
            "tools/das_idl/das_idl_batch_gen.py",
            "cmake/DasAddIdlExport.cmake",
            "tools/das_idl/test_csharp_batch_config.py",
            "tools/das_idl/test_csharp_generator.py",
            "tools/das_idl/test_csharp_host_bridge_boundaries.py",
            "tools/das_idl/swig_lang_generator_base.py",
            "tools/das_idl/swig_java_generator.py",
            "SWIG/ExportAll.i",
        }
        forbidden_tokens = {
            "CSharpSwigGenerator",
            "swig_csharp_generator",
        }
        violations = []
        for path in _iter_source_files(
            "cmake", "3rdParty", "SWIG", "tools/das_idl"
        ):
            rel = _rel(path)
            if rel in allowed_csharp_paths:
                continue
            text = _read_text(path)
            for token in forbidden_tokens:
                if token in text:
                    violations.append(f"{rel}: {token}")

        self.assertEqual(
            violations,
            [],
            "EXPORT_CSHARP must not enter _NEED_SWIG or SWIG cleanup per D-79-06",
        )

    def test_csharp_package_cmake_exposes_final_targets(self):
        cmake_path = ROOT / "das" / "Plugins" / "CSharpTestPlugin" / "CMakeLists.txt"
        text = _read_text(cmake_path)

        for token in (
            "CSharpTestPluginModernBuild",
            "CSharpTestPluginModernPackage",
            "CSharpTestPluginModernIpcPackage",
            "CSharpTestPluginNet48Build",
            "CSharpTestPluginNet48Package",
            "CSharpTestPluginNet48IpcPackage",
            "CSharpTestPluginGeneratedNativeSupport",
            "CSharpTestPluginGeneratedNativeSupportCompile",
            "DasCoreCSharpExport",
            "CSharpTestPlugin.net8.json.in",
            "CSharpTestPlugin.net48.json.in",
            "$<TARGET_FILE:CSharpTestPluginGeneratedNativeSupport>",
            "$<TARGET_FILE_NAME:CSharpTestPluginGeneratedNativeSupport>",
            "DAS_CORE_CSHARP_NATIVE_SUPPORT_MODULE_NAME",
            "OUTPUT_NAME ${DAS_CORE_CSHARP_NATIVE_SUPPORT_MODULE_NAME}",
            "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/plugins",
        ):
            self.assertIn(token, text)

        self.assertIsNotNone(
            re.search(
                r"if\(DAS_CSHARP_BUILD_MODERN\)\s+"
                r"das_add_csharp_test_plugin_ipc_package\(\s+"
                r"CSharpTestPluginModernIpcPackage",
                text,
                re.S,
            ),
            "modern IPC package target must stay behind the modern C# build gate",
        )
        self.assertIsNotNone(
            re.search(
                r"if\(DAS_CSHARP_BUILD_NET48\).*?"
                r"das_add_csharp_test_plugin_ipc_package\(\s+"
                r"CSharpTestPluginNet48IpcPackage",
                text,
                re.S,
            ),
            "net48 IPC package target must stay behind the net48 C# build gate",
        )

    def test_csharp_test_plugin_owns_project_and_includes_generated_sources(self):
        project_path = (
            ROOT / "das" / "Plugins" / "CSharpTestPlugin" / "CSharpTestPlugin.csproj"
        )
        text = _read_text(project_path)

        self.assertIn(
            r'Include="$(DasGeneratedOutputDir)\Das.Generated\**\*.cs"',
            text,
        )
        self.assertIn(r'Compile Remove="bin\**;obj\**"', text)
        self.assertNotIn("ProjectReference", text)
        self.assertNotIn("DasGeneratedProject", text)

    def test_csharp_test_plugin_resolver_uses_generated_module_constants(self):
        bootstrap_path = (
            ROOT / "das" / "Plugins" / "CSharpTestPlugin" / "src" / "Bootstrap.cs"
        )
        text = _read_text(bootstrap_path)

        self.assertIn("NativeModules.DAS_CSHARP_NATIVE_SUPPORT_MODULE", text)
        self.assertIn("NativeModules.DAS_NATIVE_MODULE", text)
        self.assertNotIn('libraryName == "Das' + 'CSharpNativeSupport"', text)
        self.assertNotIn('libraryName == "das"', text)
        self.assertNotIn("Das.CSharp.Runtime.dll", text)
        self.assertNotIn("NuGet", text)

    def test_csharp_test_plugin_directly_subclasses_generated_wrappers(self):
        source_paths = [
            ROOT / "das" / "Plugins" / "CSharpTestPlugin" / "src" / "PluginPackage.cs",
            ROOT / "das" / "Plugins" / "CSharpTestPlugin" / "src" / "Component.cs",
        ]
        combined = "\n".join(_read_text(path) for path in source_paths)

        self.assertIn("public sealed class PluginPackage : IDasPluginPackage", combined)
        self.assertIn("public sealed class Component : IDasComponent", combined)
        self.assertIn("(DasResult Result, IDasVariantVector ResultValue)", combined)
        for result_type in (
            "IDasPluginPackageCreateFeatureInterfaceResult",
            "IDasComponentFactoryCreateInstanceResult",
            "IDasComponentDispatchResult",
        ):
            with self.subTest(result_type=result_type):
                self.assertNotIn(result_type, combined)

        self.assertNotIn("out IntPtr", combined)
        self.assertNotIn("out System.IntPtr", combined)
        self.assertNotIn(".Handle", combined)

    def test_ipc_multiprocess_cmake_depends_on_csharp_packages_conditionally(self):
        cmake_path = ROOT / "das" / "IpcMultiProcessTest" / "CMakeLists.txt"
        text = _read_text(cmake_path)

        for target, definition in (
            ("CSharpTestPluginModernIpcPackage", "DAS_CSHARP_BUILD_MODERN"),
            ("CSharpTestPluginNet48IpcPackage", "DAS_CSHARP_BUILD_NET48"),
        ):
            with self.subTest(target=target):
                match = re.search(
                    rf"if\(TARGET {target}\)(.*?)endif\(\)",
                    text,
                    re.S,
                )
                self.assertIsNotNone(match)
                block = match.group(1)
                self.assertIn(
                    f"add_dependencies(\n            ${{IpcMultiProcessTest_NAME}}\n            {target})",
                    block,
                )
                self.assertIn("target_compile_definitions(", block)
                self.assertIn(definition, block)

    def test_ipc_test_config_exposes_csharp_package_manifest_helper(self):
        config_path = (
            ROOT / "das" / "IpcMultiProcessTest" / "test" / "IpcTestConfig.h"
        )
        text = _read_text(config_path)

        self.assertIn("GetCSharpTestPluginJsonPath", text)
        self.assertIn('package_dir / "CSharpTestPlugin.json"', text)
        self.assertIn('DAS_FMT_NS::format("{}.json", package_name)', text)
        self.assertIn('package_dir / "manifest.json"', text)
        self.assertIn("C# plugin JSON for package", text)
        self.assertNotIn("Das.CSharp.Runtime.dll", text)


if __name__ == "__main__":
    unittest.main()
