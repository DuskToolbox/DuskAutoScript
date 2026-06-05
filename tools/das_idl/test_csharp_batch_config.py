import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).parent
REPO_ROOT = SCRIPT_DIR.parent.parent


def _write_minimal_idl(idl_dir: Path) -> None:
    idl_dir.mkdir(parents=True, exist_ok=True)
    (idl_dir / "Core.idl").write_text(
        """
        namespace Das {
            [uuid("11111111-2222-3333-4444-555555555555")]
            interface IDasSample : IDasBase {
                DasResult GetName([out] IDasReadOnlyString** pp_out_name);
            }
        }
        """,
        encoding="utf-8",
    )


class TestCSharpBatchConfig(unittest.TestCase):
    def _run_gen_config(
        self, temp: Path, *extra_args: str
    ) -> subprocess.CompletedProcess:
        idl_dir = temp / "idl"
        _write_minimal_idl(idl_dir)
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT_DIR / "gen_batch_config.py"),
                "--output",
                str(temp / "batch.json"),
                "--idl-dir",
                str(idl_dir),
                "--idl-files",
                "Core.idl",
                "--raw-output-dir",
                str(temp / "abi"),
                "--wrapper-output-dir",
                str(temp / "wrapper"),
                "--header-output-dir",
                str(temp / "header"),
                *extra_args,
            ],
            capture_output=True,
            text=True,
        )

    def test_csharp_only_language_does_not_enable_swig_task(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)

            result = self._run_gen_config(
                temp,
                "--languages",
                "CSharp",
                "--swig-output-dir",
                str(temp / "swig"),
                "--csharp-output-dir",
                str(temp / "csharp"),
                "--csharp-namespace-root",
                "Das.Generated",
                "--csharp-package-name",
                "Das.Generated",
                "--csharp-project-name",
                "DasGenerated",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            config = json.loads((temp / "batch.json").read_text(encoding="utf-8"))
            self.assertEqual(len(config["tasks"]), 1)
            task = config["tasks"][0]
            self.assertNotIn("--swig-output-dir", task)
            self.assertNotIn("--swig", task)

    def test_python_and_java_languages_still_enable_swig_tasks(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)

            result = self._run_gen_config(
                temp,
                "--languages",
                "Python",
                "Java",
                "--swig-output-dir",
                str(temp / "swig"),
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            config = json.loads((temp / "batch.json").read_text(encoding="utf-8"))
            self.assertEqual(len(config["tasks"]), 1)
            task = config["tasks"][0]
            self.assertEqual(task["--swig-output-dir"], str(temp / "swig"))
            self.assertTrue(task["--swig"])

    def test_csharp_reduce_config_writes_explicit_contract(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)

            result = self._run_gen_config(
                temp,
                "--languages",
                "CSharp",
                "--csharp-output-dir",
                str(temp / "csharp"),
                "--csharp-namespace-root",
                "Das.Generated",
                "--csharp-package-name",
                "Das.Generated",
                "--csharp-project-name",
                "DasGenerated",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            config = json.loads((temp / "batch.json").read_text(encoding="utf-8"))
            reduce_config = config["reduce"]
            self.assertEqual(reduce_config["csharp_output_dir"], str(temp / "csharp"))
            self.assertEqual(reduce_config["csharp_namespace_root"], "Das.Generated")
            self.assertEqual(reduce_config["csharp_package_name"], "Das.Generated")
            self.assertEqual(reduce_config["csharp_project_name"], "DasGenerated")
            self.assertEqual(reduce_config["csharp_idl_dir"], str(temp / "idl"))
            self.assertEqual(reduce_config["csharp_idl_files"], ["Core.idl"])

    def test_csharp_reduce_names_are_required(self):
        cases = [
            (
                [
                    "--languages",
                    "CSharp",
                    "--csharp-output-dir",
                    "csharp",
                    "--csharp-package-name",
                    "Das.Generated",
                    "--csharp-project-name",
                    "DasGenerated",
                ],
                "--csharp-namespace-root",
            ),
            (
                [
                    "--languages",
                    "CSharp",
                    "--csharp-output-dir",
                    "csharp",
                    "--csharp-namespace-root",
                    "Das.Generated",
                    "--csharp-project-name",
                    "DasGenerated",
                ],
                "--csharp-package-name",
            ),
            (
                [
                    "--languages",
                    "CSharp",
                    "--csharp-output-dir",
                    "csharp",
                    "--csharp-namespace-root",
                    "Das.Generated",
                    "--csharp-package-name",
                    "Das.Generated",
                ],
                "--csharp-project-name",
            ),
        ]

        for extra_args, missing_flag in cases:
            with self.subTest(missing_flag=missing_flag):
                with tempfile.TemporaryDirectory() as temp_dir:
                    result = self._run_gen_config(Path(temp_dir), *extra_args)

                self.assertEqual(result.returncode, 2)
                self.assertIn(missing_flag, result.stderr)

    def test_batch_list_outputs_includes_csharp_reduce_files_without_swig(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            csharp_dir = temp / "csharp"

            result = self._run_gen_config(
                temp,
                "--languages",
                "CSharp",
                "--swig-output-dir",
                str(temp / "swig"),
                "--csharp-output-dir",
                str(csharp_dir),
                "--csharp-namespace-root",
                "Das.Generated",
                "--csharp-package-name",
                "Das.Generated",
                "--csharp-project-name",
                "DasGenerated",
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            list_result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT_DIR / "das_idl_batch_gen.py"),
                    "--config",
                    str(temp / "batch.json"),
                    "--list-outputs",
                ],
                capture_output=True,
                text=True,
            )

            self.assertEqual(list_result.returncode, 0, list_result.stderr)
            outputs = list_result.stdout.splitlines()
            csharp_outputs = [item for item in outputs if "/csharp/" in item]
            self.assertCountEqual(
                csharp_outputs,
                [
                    f"{csharp_dir}/DasGenerated.csproj".replace("\\", "/"),
                    f"{csharp_dir}/Das.Generated/DasResult.cs".replace("\\", "/"),
                    f"{csharp_dir}/Das.Generated/DasException.cs".replace("\\", "/"),
                    f"{csharp_dir}/Das.Generated/Abi/DasGuid.cs".replace("\\", "/"),
                    f"{csharp_dir}/Das.Generated/Abi/NativeMethods.cs".replace("\\", "/"),
                    f"{csharp_dir}/Das.Generated/Wrappers/IDasSample.cs".replace("\\", "/"),
                    f"{csharp_dir}/Das.Generated/Directors/IDasSampleDirector.cs".replace("\\", "/"),
                    f"{csharp_dir}/Das.Generated/Results/IDasSampleResults.cs".replace("\\", "/"),
                    f"{csharp_dir}/Native/DasCSharpDirectorSupport.h".replace("\\", "/"),
                    f"{csharp_dir}/Native/DasCSharpDirectorSupport.cpp".replace("\\", "/"),
                ],
            )
            self.assertEqual(len(csharp_outputs), len(set(csharp_outputs)))
            forbidden_tokens = ("swig", "SWIGCSHARP", "CSharpSwigGenerator")
            self.assertFalse(
                any(
                    token in output
                    for output in outputs
                    for token in forbidden_tokens
                ),
                outputs,
            )

    def test_csharp_only_cmake_route_cannot_require_swig(self):
        cmake_text = (REPO_ROOT / "cmake" / "DasAddIdlExport.cmake").read_text(
            encoding="utf-8"
        )
        self.assertIn('_LANG_LOWER STREQUAL "python"', cmake_text)
        self.assertIn('_LANG_LOWER STREQUAL "java"', cmake_text)
        self.assertIn("C# is a pure reduce-owned generator", cmake_text)
        self.assertIn("Registered C# pure reduce generator outputs", cmake_text)
        self.assertNotRegex(
            cmake_text,
            r"_NEED_SWIG[\s\S]*?_LANG_LOWER STREQUAL \"csharp\"[\s\S]*?set\(_NEED_SWIG TRUE\)",
        )
        self.assertNotRegex(
            cmake_text,
            r"find_package\(SWIG REQUIRED\)[\s\S]*?_LANG_NAME \"CSharp\"",
        )
        self.assertNotIn('set(_LANG_NAME "CSharp")', cmake_text)
        for language in ("csharp", "lua", "node"):
            self.assertRegex(
                cmake_text,
                rf'_LANG_LOWER STREQUAL "{language}"[\s\S]*?continue\(\)',
            )

    def test_third_party_swig_discovery_excludes_csharp(self):
        third_party_text = (REPO_ROOT / "3rdParty" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn("das_check_language_export(JAVA", third_party_text)
        self.assertIn("das_check_language_export(PYTHON", third_party_text)
        self.assertNotRegex(
            third_party_text,
            r"das_check_language_export\s*\(\s*CSHARP\b",
        )
        self.assertIn(
            "find_package(SWIG 4.1.1 COMPONENTS ${DAS_EXPORT_LANGUAGES_LIST} REQUIRED)",
            third_party_text,
        )

    def test_batch_execution_runs_csharp_reduce_phase(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            csharp_dir = temp / "csharp"

            result = self._run_gen_config(
                temp,
                "--languages",
                "CSharp",
                "--csharp-output-dir",
                str(csharp_dir),
                "--csharp-namespace-root",
                "Das.Generated",
                "--csharp-package-name",
                "Das.Generated",
                "--csharp-project-name",
                "DasGenerated",
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            batch_result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT_DIR / "das_idl_batch_gen.py"),
                    "--config",
                    str(temp / "batch.json"),
                ],
                capture_output=True,
                text=True,
            )

            self.assertEqual(batch_result.returncode, 0, batch_result.stderr)
            self.assertIn("CSharp Binding", batch_result.stdout)
            self.assertTrue((csharp_dir / "DasGenerated.csproj").exists())
            self.assertTrue((csharp_dir / "Das.Generated" / "DasResult.cs").exists())
            self.assertTrue(
                (csharp_dir / "Das.Generated" / "Wrappers" / "IDasSample.cs").exists()
            )


if __name__ == "__main__":
    unittest.main()
