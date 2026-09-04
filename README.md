# RifeOFX

Native OpenFX frame interpolation for DaVinci Resolve on Windows, using RIFE,
NCNN and Vulkan. No Python or PyTorch runtime is required.

## Current status

The plugin has been tested with DaVinci Resolve Free and an NVIDIA RTX 3080.
The current release validates these models:

- RIFE 4.6
- RIFE 4.22 Lite
- RIFE 4.25 Lite
- RIFE 4.25
- RIFE 4.26
- RIFE 4.26 Large

The OFX effect appears as **RIFE Frame Interpolator** in the **Open Source AI**
category. It is a temporal OpenFX effect: for each output time it converts the
render time into a position inside the original media, then explicitly requests
the two source frames that bracket that position through the OpenFX temporal
clip access, and runs RIFE with the fractional timestep between them. See
[docs/architecture.md](docs/architecture.md) and
[docs/temporal-test.md](docs/temporal-test.md).

## Installation

Download the Windows release ZIP, extract it, and run `install_plugin.bat` as
administrator while DaVinci Resolve is closed. Restart Resolve and add the
effect from Effects Library > Open FX.

The release bundle is self-contained: all six model directories are included.
The model is loaded on demand and only the selected model is kept active by the
engine.

## Controls

- Model: the RIFE model to use
- Detected Framerate (read-only, reported by the host)
- Source Framerate: original media cadence, override it when the host only
  exposes the conformed timeline rate
- Use Timeline Framerate / Target Framerate
- GPU Device
- Debug
- Phase Override: debug escape hatch when cadence calibration reports ambiguous

## Build from source

Prerequisites:

- Windows 11 x64
- Visual Studio 2022 C++ build tools
- CMake 3.24+
- LunarG Vulkan SDK
- Git with submodule support

Clone with submodules:

```powershell
git clone --recurse-submodules https://github.com/Gyom03/rife-ofx.git
cd rife-ofx
```

Configure and build the normal plugin:

```powershell
cmake -S . -B build-rife -G "Visual Studio 17 2022" -A x64 `
  -DRIFE_ENABLE_INFERENCE=ON `
  -DVulkan_INCLUDE_DIR="$env:VULKAN_SDK\Include" `
  -DVulkan_LIBRARY="$env:VULKAN_SDK\Lib\vulkan-1.lib"
cmake --build build-rife --config Release --parallel
ctest --test-dir build-rife -C Release --output-on-failure
```

`ctest` runs `CadenceMappingTest`, which covers the rational cadence ratio, the
phase calibration and the arithmetic mapping without an OFX host or a GPU.

To build a bundle containing all local model files, provide a model root with
the six directories named exactly as in `models/registry.csv`:

```powershell
cmake -S . -B build-release -G "Visual Studio 17 2022" -A x64 `
  -DRIFE_ENABLE_INFERENCE=ON `
  -DRIFE_BUNDLE_MODELS_ROOT="$pwd\external-models" `
  -DVulkan_INCLUDE_DIR="$env:VULKAN_SDK\Include" `
  -DVulkan_LIBRARY="$env:VULKAN_SDK\Lib\vulkan-1.lib"
cmake --build build-release --config Release --parallel
```

The output is `RifeFrameInterpolator.ofx.bundle`.

## License

RifeOFX source code is MIT licensed. See [LICENSE](LICENSE) and
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for third-party code,
model attribution and redistribution notes.
