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
    def compile_fixture(self, source_path, fixture_name, names, extra_flags=()):
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
                            *extra_flags, str(cpp), "-o", str(binary)], check=True)
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

    def test_shadow_depth_guard_and_query_budget(self):
        self.compile_fixture("GWToolboxdll/Modules/Weather/Skybox.cpp",
                             "weather_depth_fixture.cpp",
                             ("PrepareShadowDraw", "RestoreReplayDepthWrites"))

    def test_shadow_resources_are_sized_and_created_on_demand(self):
        self.compile_fixture("GWToolboxdll/Modules/Weather/Skybox.cpp",
                             "weather_shadow_resources_fixture.cpp", (
                                 "ReleaseShadowPreviewResources", "EnsureShadowPreviewResources",
                                 "EnsureTerrainReplayShaders",
                             ))

    def test_particle_only_weather_does_not_initialize_atmosphere(self):
        self.compile_fixture("GWToolboxdll/Modules/WeatherModule.cpp",
                             "weather_initialization_fixture.cpp",
                             ("WeatherModule::Initialize", "WeatherModule::LoadSettings"))

    def test_device_hooks_expand_only_when_shadows_need_them(self):
        self.compile_fixture("GWToolboxdll/Modules/Weather/Skybox.cpp",
                             "weather_device_hooks_fixture.cpp",
                             ("EnsureD3DReplayHooks",))

    def test_native_camera_geometry_is_independent_of_colour_precision(self):
        sdk = Path(os.environ.get("XWIN_SDK", ROOT / ".xwin-toolchain/xwin-sdk"))
        if not (sdk / "sdk/include/um/DirectXMath.h").is_file():
            self.skipTest("Provision the xwin SDK to run the DirectXMath camera fixture")
        self.compile_fixture("GWToolboxdll/Utils/ShadowCamera.cpp",
                             "weather_shadow_camera_fixture.cpp",
                             ("IsFinite", "ShadowCamera::BuildDirectional"),
                             ("-I", str(ROOT / "GWToolboxdll"),
                              "-isystem", str(sdk / "sdk/include/um"),
                              "-idirafter", str(sdk / "crt/include")))


if __name__ == "__main__":
    unittest.main()
