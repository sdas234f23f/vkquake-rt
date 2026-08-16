# Quake: Ray Traced

Quake: Ray Traced adds a path tracing renderer to id Software's [Quake](https://en.wikipedia.org/wiki/Quake_(video_game)) using the [RayTracedGL1](https://github.com/sultim-t/RayTracedGL1) library.

Quake: Ray Traced is based on the [vkQuake](https://github.com/Novum/vkQuake) — a port of QuakeSpasm to Vulkan API.

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
* [Vulkan SDK](https://vulkan.lunarg.com/sdk/home)
* [RayTracedGL1](https://github.com/sultim-t/RayTracedGL1) built library
* GPU with ray tracing support

Steps:

1. Clone the repository:

   ```
   git clone --recursive https://github.com/sdas234f23f/vkquake-rt.git
   ```

2. Build [RayTracedGL1](https://github.com/sultim-t/RayTracedGL1) (the `quake-fsr31-support` branch) with CMake.

3. Set the `vkpt_SDK_PATH` environment variable to the RayTracedGL1 repository root
   (it must contain `Include/vkpt/vkpt.h` and `Build/RayTracedGL1.lib`):

   ```
   setx vkpt_SDK_PATH "C:\path\to\RayTracedGL1"
   ```

4. Configure and build (open a new terminal after `setx`):

   ```
   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

   For a debug build use `-DCMAKE_BUILD_TYPE=Debug`.

5. Download the [Fidelity SDK 1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v1.1.4/FidelityFX-SDK-v1.1.4.zip) and copy amd_fidelityfx_vk.dll to the folder with `vkquake.exe`.

5. Run the game:

   ```
   build\vkquake.exe
   ```

   `SDL2.dll`, `RayTracedGL1.dll` and all codec DLLs are copied next to `vkquake.exe` automatically during the build.
