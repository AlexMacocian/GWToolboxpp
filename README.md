# GWToolbox++

## A set of tools for Guild Wars Players

If you are here to check toolbox features or for a download link, go to [https://gwtoolbox.com](https://gwtoolbox.com). Stay here and keep reading for information on how to download and build from source.

## How to download, build, and run
### Requirements
* Visual Studio 2026 version 18.0+. You can download [Visual Studio Community](https://visualstudio.microsoft.com/vs/community/) for free. You will also need the "Desktop development with C++" package.
* C++23 compatible v144 MSVC Platform Toolset
* Windows 11 SDK
* CMake 3.29 or higher. This is integrated in the [Visual Studio Developer PowerShell](https://learn.microsoft.com/en-us/visualstudio/ide/reference/command-prompt-powershell?view=vs-2022). Alternatively download the latest version from [https://cmake.org/download/](https://cmake.org/download/).
* vcpkg. This is integrated in the [Visual Studio Developer PowerShell](https://learn.microsoft.com/en-us/visualstudio/ide/reference/command-prompt-powershell?view=vs-2022). Alternatively download the latest version from [https://github.com/microsoft/vcpkg/releases/](https://github.com/microsoft/vcpkg/releases/latest).
* [Git](https://git-scm.com/)

1. Open **Visual Studio Developer PowerShell**. Use all the following commands: 

2. Clone the repository: 
`git clone https://github.com/gwdevhub/GWToolboxpp.git`

3. Navigate to the GWToolboxpp folder: 
`cd GWToolboxpp`

4. Build: `cmake --preset=vcpkg`

5. Open: `cmake --open build`

6. Build the solution. `cmake --build build --config RelWithDebInfo`

7. Run.

## Building on Linux

`clang` targets `i686-pc-windows-msvc` directly, linking with `lld-link`, against MSVC CRT
and Windows SDK headers/libs fetched by [xwin](https://github.com/Jake-Shadle/xwin) from
Microsoft's official installer manifests. C++ compilation, resource compilation, linking
and shader compilation run natively on Linux, without Wine. SM3 shaders are compiled by
[vkd3d-shader](https://gitlab.winehq.org/wine/vkd3d) in place of `fxc.exe`, since `fxc` is
the only Microsoft compiler still emitting shader model 1-3 and `dxc` dropped everything
below SM6. The toolchain pins vkd3d-shader 2.0 at `be62407e706ca155a36e7f7dd4422b479bca32a9`,
matching the compiler used by the original Rebirth shaders. The small CLI patch preserves
D3DCompile's global-uniform and floating-point defaults; it does not modify any HLSL.
Older vkd3d 1.19 cannot compile these shaders' SM3 loops.

**With Docker** - the only requirement is [Docker](https://docs.docker.com/get-docker/):

```sh
./scripts/build-xwin.sh
```

The first run builds the image (downloads the SDK headers/libs and builds vkd3d, expect it to
take a while), then configures and builds `GWToolboxdll` into `bin/GWToolboxdll.dll`. Later
runs reuse the cached image and only rebuild changed files.

Options (see `./scripts/build-xwin.sh --help`):
* `--host` - build directly on Linux without Docker
* `--target <name>` - build a different CMake target (default: `GWToolboxdll`; use `all` for everything)
* `--config <Debug|RelWithDebInfo|Release>` - CMake config to build (default: `RelWithDebInfo`)
* `--jobs <n>` - parallel build jobs (default: all cores)
* `--shell` - drop into a shell in the build container instead of building
* `--rebuild-image` - force a clean rebuild of the Docker image

The container runs as root; the script hands ownership of `build-xwin/`, `bin/` and generated
shader headers back to your user, including after a failed build. Every invocation reconfigures the gitignored
`build-xwin/` directory, so changes to `--config` and `--cmake-arg` take effect immediately.

On a Windows host, `scripts\build-xwin.ps1` drives the same container through Docker Desktop
(`-Config`, `-Target`, `-Jobs`, `-Shell`, `-RebuildImage`). Use it to reproduce a CI result or
to check a change builds clean under clang - for ordinary Windows development build with
Visual Studio or `build-clang.bat` instead.

**Directly on the host** - needs `clang` **19 or newer** (the MSVC STL rejects older ones
outright with `error STL1000`; note Ubuntu 24.04 still ships 18), `lld`, `llvm` (for
`llvm-rc`/`llvm-lib`/`llvm-mt`), `cmake` >= 3.29, `ninja`, `python3`, and a bootstrapped
`vcpkg` in `$VCPKG_ROOT`. Some distros do not ship a `clang-cl`; it is the same binary as
`clang` selected by name, so `ln -s $(command -v clang) /usr/local/bin/clang-cl` is enough.
Building vkd3d also needs a native C compiler, `make`, `pkg-config`, `flex`, `bison`,
Autotools, native `widl`, Perl's `JSON` module and Vulkan headers (`wine64-tools`,
`libjson-perl` and `libvulkan-dev` on Debian/Ubuntu). `widl` is a Linux executable:
neither a Wine process nor a Vulkan runtime is needed.

```sh
export VCPKG_ROOT=/path/to/vcpkg
./scripts/build-xwin.sh --host --config RelWithDebInfo --jobs 6
file bin/GWToolboxdll.dll
```

The optional shader-only Wine path accepts `fxc.exe` from the Windows SDK:

```sh
./scripts/build-xwin.sh --host --config RelWithDebInfo --jobs 6 \
  --cmake-arg "-DFXC=/path/to/WindowsSDK/bin/x64/fxc.exe"
```

Only HLSL compilation runs through Wine in this mode; C++ and linking still use native
`clang-cl` and `lld-link`. The shader runner preserves `WINEDLLOVERRIDES` and uses a
dedicated `.xwin-toolchain/wine-shaders` prefix unless `WINEPREFIX` is set. Rebirth's Wine
build used the builtin vkd3d-backed D3DCompile implementation, not Microsoft's native
`d3dcompiler_47.dll`; forcing the latter rejects some unchanged SM3 shaders.

The first invocation provisions `.xwin-toolchain/`; subsequent builds reuse it.
The output is `bin/GWToolboxdll.dll`, a **PE32 DLL for Intel 80386**, not a native Linux
library or a 64-bit Windows DLL. CMake also rejects targets whose pointer size is not
four bytes. The script does not install or deploy the DLL.

To build against an unmerged sibling GWCA checkout, including its local changes:

```sh
VCPKG_ROOT=/path/to/vcpkg ./scripts/build-xwin.sh --host --jobs 6 --gwca-source ../GWCA
```

This builds GWCA with the same xwin SDK, stages its matching DLL/import library/headers
in `Dependencies/GWCA`, then builds Toolbox and embeds that exact DLL. Neither build
runs WASM or needs GitHub artifacts.

### Weather atmosphere test iteration

Enable **Weather**, then opt in to **Atmospheric sky and lighting** in its settings.
This enables the replacement sky, lighting, native caster shadows and water; existing
`/weather` and `/climate` controls remain available with atmosphere off. Sky tuning is
session-only. Unload standalone Rebirth before testing this port. Rezone when comparing
baked terrain shadows: tiles already streamed with cleared shadows cannot be restored
by toggling the effect off.

Local xwin builds write crash dumps even when the updater considers the build outdated
or plugins are loaded. Keep the matching `bin/GWToolboxdll.pdb` with your test DLL.
Release builds retain the normal reporting restrictions; pass
`--cmake-arg -DGWTOOLBOX_ALLOW_UNSUPPORTED_CRASH_DUMPS=OFF` to retain them in xwin too.

Caveats: vkd3d-shader has no SM1-3 optimiser, so the shader bytecode is longer than `fxc`'s
(register allocation is unaffected). Releases are still cut on Windows with MSVC - validate
rendering changes in-game before shipping a clang-built DLL.

## Notes
* GWToolbox compiles as a DLL (`GWToolboxdll.dll`) and EXE (`GWToolbox.exe`). The exe lets you select a Guild Wars Client and injects the dll, but you can also use other dll injectors of your choice.
* By default, the launcher (`GWToolbox.exe`) will run the `GWToolboxdll.dll` in the same folder, if there is none, it will inject the one in your installation folder.
* You can use the Visual Studio debugger directly to be able to break and step through toolbox code. First launch toolbox in debug mode as normal, then go to Debug -> Attach to process, then select the Gw.exe process and click Attach. You can also attach the debugger *before* running toolbox, to debug issues during launch, but then you will have to manually launch toolbox from outside visual studio, either with `GWToolbox.exe` or `AutoItLauncher/inject.au3`. 

## How to contribute
* Create a new branch and commit the changes relevant to your contribution. Please make sure you don't commit unrelated lines.
* Run clang-format and clang-tidy on the code you wrote. .clang-tidy and .clang-format files are in the root of the repository.
* Finally, submit a pull request and let us review your code. Stay updated in the pull request page for feedback and comments.

**Quick code overview:**
There are three main kinds of Toolbox components: Modules, Widgets and Windows. Any new code is most likely an additional one of those, or some change in one. 
* Modules are components without an interface, but they do have a panel in Settings.
* Widgets are visual elements without an explicit window (border, header, etc).
* Windows are visual elements with an explicit window.

Both Widgets and Windows can also have a panel in Settings, and share common code in ToolboxUIElement, which handles saving of position, visibility, etc. If you wish to create a new window/widget, please take a look at how similar ones have already been implemented.  

**Important**: The destruction chain is as follows: SignalTerminate -> Terminate -> Destructor. Make sure to handle all destruction logic that uses other modules or interfaces with GWCA in SignalTerminate, Terminate to revert any changes to the game and only use the destructor for class scope clean up.

## Plugins
Toolbox supports plugins, meaning you can extend Toolbox functionality.
Please take note that plugins are currently a *beta* feature - plugins compiled for one version of toolbox should continue working, but may have to be recompiled

For users: put the plugin into GWToolboxpp/\<Computername\>/plugins
If you use plugins that aren't compatible with your Toolbox version, you might experience crashes.

For developers: there are a few things you should take note of:
* Two examples (Clock and InstanceTimer) will automatically be added to the solution (see CMakeLists.txt)
* Your Plugin::Initialize must call ToolboxPlugin::Initialize(ctx, fns, tbdll), otherwise you must take care of creating and destroying your own ImGui context.
* We do not guarantee API stability between versions
* If you wish to draw in your plugin, inherit from ToolboxUIPlugin. It will automatically initialise GWCA access for you (GW::Initialize()). If you create a plugin that inherits from ToolboxPlugin, you must manage that yourself. If you use GWCA, make sure the GW::Scanner points to GW.exe at the end of Plugin::Initialize().

## Credits

**[HasKha](https://github.com/HasKha)**
* Original creator of GWToolbox++.
 
**[KAOS](https://github.com/GregLando113)**
* Original creator of the GW API used, reverse engineering work.
* Several minor additions.

**[Ziox](https://github.com/reduf)**   
* Implementation of the vast majority of the chat-based features, such as custom chats and the chat timestamps.
* Major contributor to the GW API used, reverse engineering work.
 
**[Jon](https://github.com/3vcloud)**
* Implemented many new features and improvements.
* Major contributor to the GW API used, reverse engineering work.
* Current maintainer of the project.
 
**[Marc](https://github.com/henderkes)**
* Implemented many new features and improvements.
* Code quality improvements.

**[Itecka]()** 
* Original creator of a damage monitor, which inspired the toolbox damage monitor.
* Created the original implementation of the Cursor Fix.

**[Misty](https://github.com/Hour-of-the-Owl)/[DarkManic](https://github.com/DarkManic)**
* Extensive work on the site and documentation.

**Everyone who [proposed a PR](https://github.com/gwdevhub/GWToolboxpp/pulls?q=is%3Apr+is%3Amerged)**

**and everyone suggesting ideas!**


All images in `resources/icons` are from www.flaticon.com

## Sponsors

- The community of users who donate. All proceeds are invested only into server-costs, visible on OpenCollective, we do not pay ourselves.
