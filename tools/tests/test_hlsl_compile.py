import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest.mock import mock_open, patch


spec = importlib.util.spec_from_file_location("hlsl_compile", Path(__file__).parents[1] / "hlsl_compile.py")
hlsl_compile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hlsl_compile)


class HlslCompileTests(unittest.TestCase):
    def arguments(self, *options):
        return [
            "hlsl_compile.py", *options, "/nologo", "/E", "main", "/T", "ps_3_0",
            "/Vn", "test_ps", "/O3", "/Zi", "/Fh", "build/test_ps.h",
            "shaders/shader with spaces.hlsl",
        ]

    def test_native_compiler_resolves_includes_relative_to_source(self):
        result = subprocess.CompletedProcess([], 0, stdout=b"\x00\x03\xff\xff", stderr=b"")
        output = mock_open()
        with patch("sys.argv", self.arguments("--vkd3d-compiler", "/native/vkd3d-compiler")), \
                patch.object(hlsl_compile.subprocess, "run", return_value=result) as run, \
                patch.object(hlsl_compile.os, "makedirs"), patch("builtins.open", output):
            hlsl_compile.main()
        self.assertEqual(run.call_args.kwargs["cwd"], Path("shaders").resolve())
        self.assertEqual(run.call_args.args[0][-3:], ["-o", "-", str(Path("shaders/shader with spaces.hlsl").resolve())])
        self.assertIn("const BYTE test_ps[]", output().write.call_args.args[0])
        self.assertIn("  0,   3, 255, 255", output().write.call_args.args[0])

    def test_failed_native_compile_preserves_existing_header(self):
        result = subprocess.CompletedProcess([], 5, stdout=b"", stderr=b"unsupported instruction\n")
        with patch("sys.argv", self.arguments()), \
                patch.object(hlsl_compile.subprocess, "run", return_value=result), \
                patch("sys.stderr"), patch("builtins.open") as output:
            with self.assertRaises(SystemExit) as error:
                hlsl_compile.main()
        self.assertEqual(error.exception.code, 5)
        output.assert_not_called()

    def test_fxc_runs_only_shader_with_windows_relative_paths(self):
        result = subprocess.CompletedProcess([], 0)
        with patch("sys.argv", self.arguments("--fxc", "/sdk tools/fxc.exe")), \
                patch.object(hlsl_compile.subprocess, "run", return_value=result) as run, \
                patch.dict(hlsl_compile.os.environ, {
                    "WINEPREFIX": "/existing/prefix", "WINEDLLOVERRIDES": "d3dcompiler_47=b",
                }):
            with self.assertRaises(SystemExit) as error:
                hlsl_compile.main()
        self.assertEqual(error.exception.code, 0)
        command = run.call_args.args[0]
        self.assertEqual(command[:2], ["wine", "/sdk tools/fxc.exe"])
        self.assertEqual(command[-3:], ["/Fh", "..\\build\\test_ps.h", "shader with spaces.hlsl"])
        self.assertEqual(run.call_args.kwargs["env"]["WINEPREFIX"], "/existing/prefix")
        self.assertEqual(run.call_args.kwargs["env"]["WINEDLLOVERRIDES"], "d3dcompiler_47=b")

    def test_missing_profile_fails_before_running_compiler(self):
        with patch("sys.argv", ["hlsl_compile.py", "/Vn", "test_ps", "/Fh", "test.h", "test.hlsl"]), \
                patch.object(hlsl_compile.subprocess, "run") as run:
            with self.assertRaisesRegex(SystemExit, "missing required argument"):
                hlsl_compile.main()
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
