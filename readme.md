# Quake: Ray Traced

Quake: Ray Traced adds a path tracing renderer to id Software's [Quake](https://en.wikipedia.org/wiki/Quake_(video_game)).

The renderer is a Q2RTX-style ray tracer (ported from [Q2RTX](https://github.com/NVIDIA/Q2RTX)) — it is **vendored into this repository** in the `vkpt/` folder (source + shaders + KTX/FidelityFX) and built as a static library linked straight into `vkquake.exe`. There is no external renderer library dependency.

Quake: Ray Traced is based on the [vkQuake](https://github.com/Novum/vkQuake) — a port of QuakeSpasm to Vulkan API.

## What is implemented

* Q2RTX-style path traced lighting: NEE direct light (per-BSP-cluster light lists, light-selection CDF + adaptive shadow statistics) and NEE indirect light with a second diffuse bounce
* Per-BSP-cluster light lists — the world model's BSP leaves are used as clusters and the PVS is used for cluster visibility, exactly like Q2RTX (no distance-based cutoffs, occluded lights excluded per cluster)
* ASVGF denoiser (Q2RTX), checkerboard rendering, TAAU upscaler by default; FSR 2/FSR 3.1/DLSS upscalers are also available in the video menu
* Reflection/refraction, god rays (volumetric sunlight), fog volumes, procedural sky
* Q2RTX-style materials: `.mat` definitions + `.pkz` archives mounted as native search paths, automatic detection from HD texture pack suffixes (`_norm`, `_gloss`, `_luma`, `_glow`)
* Dynamic lights (torches, muzzle flashes, explosions) and map `light` entities as RT light sources
* `rt_debugflags` diagnostic views (raw unfiltered direct/indirect/specular, gradients, etc.)

The classic (non-RT) engine lighting is fully disabled in the ray-traced renderer — the ray tracer produces all the lighting (Q2RTX model). The classic renderer fallback is still available via `rt_classic_render 1`.

## Changelog

See [changelog.md](changelog.md).

## Build

The project is built with CMake and Ninja using the MSVC compiler from Visual Studio Build Tools.

### Windows

Prerequisites:

* [Git for Windows](https://github.com/git-for-windows/git/releases)
* [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/) with the "Desktop development with C++" workload
* [CMake](https://cmake.org/download/) 3.20 or newer
* [Ninja](https://ninja-build.org/)
* [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) (with `glslc`; the shaders are compiled from `vkpt/Source/Shaders`)
* GPU with ray tracing support

Steps:

1. Clone the repository:

   ```
   git clone https://github.com/sdas234f23f/vkquake-rt.git
   ```

2. (Re)build the SPIR-V shaders — optional, but after changing any shader source you must regenerate them:

   ```
   cd vkpt/Source/Shaders
   python GenerateShaders.py
   cd ../..
   Copy-Item vkpt\Build\*.spv build\Debug\ovrd\shaders\ -Force
   ```

3. Configure and build:

   ```
   .\build_win.ps1 Debug build\Debug
   ```

   (or with plain CMake: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` + `cmake --build build`; for a debug build use `-DCMAKE_BUILD_TYPE=Debug`).

   The build also deploys the override-material pack `id1/ovrd_mat.pkz` (checked in) into the build's game dir, so the ray-traced material overrides (emissive lava, ...) are always present.

4. Run the game:

   ```
   build\vkquake.exe
   ```

   `SDL2.dll` and all codec DLLs are copied next to `vkquake.exe` automatically during the build. The renderer is compiled into the executable — no external renderer DLL is needed. The `.spv` shaders and the blue noise texture are loaded from the game data (`ovrd/shaders/`, `ovrd/BlueNoise_LDR_RGBA_128.ktx2`).

## Game data

Quake 1 game files (`id1/`) are required (registered or shareware). HD texture packs can be used through `.pkz` archives or `.mat` material definitions (see `Tools/` for converters). The renderer's override-material pack `id1/ovrd_mat.pkz` is checked in and deployed into the build's game dir by `build_win.ps1`.

