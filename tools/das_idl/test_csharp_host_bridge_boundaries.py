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
    def test_csharp_swig_route_is_not_restored(self):
        self.assertFalse(
            (ROOT / "tools" / "das_idl" / "swig_csharp_generator.py").exists()
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

    def test_csharp_package_cmake_exposes_final_targets(self):
        cmake_path = ROOT / "das" / "Plugins" / "CSharpTestPlugin" / "CMakeLists.txt"
        text = _read_text(cmake_path)

        for token in (
            "CSharpTestPluginModernBuild",
            "CSharpTestPluginModernPackage",
            "CSharpTestPluginNet48Build",
            "CSharpTestPluginNet48Package",
            "CSharpTestPluginGeneratedNativeSupportCompile",
            "DasCoreCSharpExport",
            "CSharpTestPlugin.net8.json.in",
            "CSharpTestPlugin.net48.json.in",
        ):
            self.assertIn(token, text)

        self.assertNotIn("Das.CSharp.Runtime.dll", text)
        self.assertNotIn("NuGet", text)


if __name__ == "__main__":
    unittest.main()
