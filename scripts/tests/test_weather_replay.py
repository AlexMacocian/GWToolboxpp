import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function_source(source, name):
    start = re.search(r"^[ \t]*(?:[\w:*]+ )+" + re.escape(name) + r"\(", source, re.MULTILINE).start()
    opening = source.index("{", start)
    depth = 0
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|//[^\n]*|[{}]', source[opening:]):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:opening + token.end()]
    raise ValueError(f"Unclosed function: {name}")


class WeatherReplayTest(unittest.TestCase):
    def compile_fixture(self, source_path, fixture_name, names):
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        self.assertTrue(compiler, "A native C++ compiler is required")
        source = (ROOT / source_path).read_text()
        fixture = Path(__file__).with_name(fixture_name).read_text()
        functions = "\n\n".join(function_source(source, name) for name in names)
        with tempfile.TemporaryDirectory(prefix="weather-replay-") as directory:
            cpp = Path(directory) / "replay.cpp"
            binary = Path(directory) / "replay"
            cpp.write_text(fixture.replace("REPLAY_FUNCTIONS", functions))
            subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_native_replay_state_and_order(self):
        self.compile_fixture("GWToolboxdll/Modules/Weather/Skybox.cpp",
                             "weather_replay_fixture.cpp",
                             ("UpdateShadowView", "RenderShadowScene"))

    def test_shared_compositor_order_and_safe_boundaries(self):
        self.compile_fixture("GWToolboxdll/Utils/GameWorldCompositor.cpp",
                             "weather_compositor_fixture.cpp", (
                                 "RunCallbacks", "RunPreWorldCallbacks", "SetActiveWorldPrograms",
                                 "OnFrCacheRenderAll", "GameWorldCompositor::RegisterDraw",
                                 "GameWorldCompositor::UnregisterDraw",
                                 "GameWorldCompositor::RegisterPreWorldDraw",
                                 "GameWorldCompositor::UnregisterPreWorldDraw",
                             ))


if __name__ == "__main__":
    unittest.main()
