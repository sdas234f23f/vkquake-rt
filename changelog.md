# Changelog

## v3.6.0

### Removed
- **Stale `RTGL1/` folder** — two leftover files from the vkpt rename (`GenerateShaderCommon.py`, `CmQ2LightListBuild.comp`) that nothing referenced were deleted; the vendored renderer now lives entirely in `vkpt/`.

### Changed
- **Dropped all remaining RayTracedGL1/RTGL1 references** — CI now builds only the vendored vkpt renderer (no RayTracedGL1 clone, no dead `vkpt_SDK_PATH` flag); comments in the root and `vkpt/` CMakeLists no longer mention `RTGL1.dll`; the renderer dev-config default file was renamed `RayTracedGL1.txt` → `vkpt.txt`.

## v3.5.0

### Added
- **Override-material pack committed to the repo** — `id1/ovrd_mat.pkz` (81 override materials + textures) is now source-controlled instead of living only in the build dir. `build_win.ps1` deploys it into the build's game dir on every build, so the material overrides (emissive lava, etc.) can no longer silently disappear with the build dir. The game runs on the `.pkz` pack alone (the renderer's `ovrd/mat` override folder is no longer deployed).

## v3.4.0

### Added
- **Q2RTX per-BSP-cluster light lists** — the camera-centered 16³ cell grid was replaced with the real Q2RTX mechanism: the world model's BSP leaves are used as clusters and the PVS is used for cluster visibility. Each light is added to every PVS-visible cluster from its own leaf (no distance-based cutoffs), and occluded lights are excluded per cluster. The direct/indirect passes look up the light list by the BSP cluster of the hit (per-pixel cluster texture for primary hits, per-triangle cluster for bounces). The GPU light-list build (`CmQ2LightListBuild.comp`, `LightGrid`) is gone.
- **Square light patches fixed** — torch / muzzle flash / explosion lights no longer cut off at cell-grid boundaries (the light lists had a distance cutoff that bright lights exceeded, producing quadrilateral patches on walls). The classic lightmap is no longer applied as a shading layer on top of the ray-traced albedo either.
- **Classic lighting fully disabled in the RT renderer** — `R_PushDlights`, `R_RenderDynamicLightmaps` and the lightmap SHADE layer in the RT world material are all skipped unless `rt_classic_render 1`. The ray tracer produces all the lighting (Q2RTX model).
- **Cluster data in the world geometry** — every world vertex carries the BSP leaf ("cluster") of its surface (`rt_surfcluster[]` in the game; `RgVertex.cluster` in the renderer).

### Changed
- **RTGL1 renamed to vkpt** — the vendored renderer folder, public headers (`vkpt.h`/`vkptA.h`), the C++ namespace and the CMake target are now named `vkpt` (Q2RTX-style name). No functional changes.

## v3.3.0

### Added
- **Decoupled from RayTracedGL1.dll (4.7)** — the Q2RTX-style ray-traced renderer is now vendored into the repository (`vkpt/` folder: source + shaders + KTX/FidelityFX) and built as a static library linked into `vkquake.exe`.
- **The game no longer depends on the RayTracedGL1 library** — `RayTracedGL1.dll` is not required at runtime anymore; the renderer is compiled straight into `vkquake.exe`. The public `RG_*` API is kept as an internal interface (`RG_STATIC`); the `.spv` shaders and blue noise are still loaded from the game data (`ovrd/shaders/`, `ovrd/BlueNoise_LDR_RGBA_128.ktx2`). The renderer's own changelog is preserved at the bottom of this file.

### Changed
- **The legacy (pre-Q2RTX) renderer was removed** — the Q2RTX-style core is now the only renderer (`rt_core_q2rtx` cvar removed; `coreQ2RTX` is always on). The legacy SVGF denoiser path, legacy screen-space volumetric (`rt_volume_type` no longer renders) and the legacy reflection/refraction pass are gone. Kept: the ASVGF denoiser, the TAAU upscaler (default), **FSR 2/FSR 3.1/DLSS upscalers still available** (they run on the Q2RTX final image when selected in the video menu), god rays, fog volumes, procedural sky, ReSTIR lighting, and the classic (non-RT) renderer fallback.
- Fixed an intermittent AMD overlay flicker (gone together with the legacy render path).

## v3.2.0

### Added
- **Master brightness** — `rt_brightness` cvar scales all light sources (sun, dynamic, world, ambient), the volumetric light and the sky
- **Sky display color** — `rt_sky_color_r/g/b` tints (or blackens) the sky independently of the sun light (`rt_sky_light_*`); applied to every sky path (solid sky, skybox, cloud layers) and to the procedural sky
- **Sky brightness** — `rt_sky_brightness` cvar scales the sky display for all sky types
- **Light color tint** — `rt_light_color_r/g/b` applies an RGB tint to every light source (sun, dynamic, world, ambient, volumetric)

### Fixed
- Material lookup now normalizes BSP-sourced texture names (`maps/<map>.bsp:*lava1` → `textures/#lava1`, `maps/<map>.bsp:+0_med100` → `textures/+0_med100`, sub-directories like `e1u1/foo` included), so `.mat`/auto-detected materials apply to world surfaces that have no external HD base texture
- Sky tint/brightness is now applied to the skybox and cloud layers as well (previously only the solid-sky polys were modulated), so `rt_sky_color`/`rt_sky_brightness`/`rt_brightness` work on every sky type

## v3.1.0

### Added
- **Q2RTX-style materials** — material system for ray-traced rendering:
  - `.pkz` archives are mounted as native engine search paths and read like `.PAK` files (`COM_*` file API, sounds, textures and `.mat` all work from them)
  - `.mat` material definitions (Q2RTX format) loaded from `materials/*.mat` (on disk or inside `.pkz`), with map-specific overrides via `materials/<map>.mat`
  - Automatic material detection from HD texture-pack suffixes: `_norm` (normal map), `_gloss` (gloss → roughness), `_luma`/`_glow` (emissive), `_bump`
  - JPG/PNG decoding (stb_image) in the engine image loader and in material textures
  - `rt_mat` console command (inspect materials) and `rt_materials` cvar (on/off)
  - `Tools/convert_ovrd_mat.py` — converts vkpt override materials (`ovrd/mat/*.ktx2`, RGBA8/BC5/BC7) into a ready-to-use `.pkz` (textures + `.mat`)
  - `Tools/gen_materials.py` — generates `.mat` files from HD texture packs using the `_norm`/`_gloss`/`_luma`/`_glow` suffix convention
- **Crash log** — on an unhandled exception a `crash.log` (exception code, faulting address, module-relative stack trace) is written next to the executable

### Fixed
- `.mat` parser no longer treats `#` inside Quake texture names (e.g. `textures/#lava1`) as a comment
- Material lookup preserves model skin names like `progs/armor.mdl:frame0` (per-frame model materials)
- Unsupported model formats (e.g. MD3 `IDP3` models from HD packs) are skipped with a console warning instead of aborting the game or the level
- Client model precache no longer aborts when a precached model is missing or unsupported
- Entity rendering skips entities without a model instead of crashing
- Sky surfaces are no longer uploaded as opaque ray-traced geometry (they occluded the sun in the god-rays shadow map and blocked primary rays from reaching the sky)

## v3.0.0

### Added
- **Q2RTX core path switch** — `rt_core_q2rtx` cvar enables the new Q2RTX-style rendering core in vkpt (ASVGF denoiser + checkerboard interleave + TAAU)
- **Fog volumes** — port of the Q2RTX `fog` console command: define up to 8 axis-aligned fog boxes with `fog -v <i> -a <x,y,z|here> -b ... -c <r,g,b> -d <dist> -f <face>`; print (`-p`), reset (`-r`) and clear-all (`-R`) supported
- **Sun presets** — `rt_sun_preset` cvar (manual, warm, daylight, neutral, sunset, cold, Q1 purple, cold blue); sun color now lives in separate `rt_sky_light_r/g/b` cvars, decoupled from `rt_globallight_*`
- **Physical sky support** — `rt_physical_sky` enables the procedural atmosphere in vkpt; `rt_sky_tint` and cloud cvars control the look
- Volumetric menu: left/right arrows now cycle through off/simple/sky modes in both directions

## v2.2.0

### Added
- **Bloom option** — `rt_bloom` cvar with a video-menu entry toggling bloom independently of the renderer

### Fixed
- Classic render: RT portals are now disabled and the world is reloaded when switching the renderer

## v2.1.0

### Added
- **AMD FSR 3.1 upscaler** support (`rt_upscale_fsr31` cvar) with quality presets: Native AA, Quality, Balanced, Performance, Ultra Performance
- **RT dynamic lights for explosions** — rocket/grenade explosions now emit spherical lights in ray-traced mode (previously only worked in classic renderer)
- **Redesigned video menu**: upscalers (FSR 2.0, FSR 3.1, DLSS) grouped under a single `Upscaler` selector with a separate `Preset` option. Only available upscalers are shown for the current GPU
- **CMake build system** — the project now builds with CMake + Ninja (MSVC from Visual Studio Build Tools); `build_win.ps1` automates the build. Visual Studio solution, Meson and Makefiles were removed

### Fixed
- RT explosion lights were missing in ray-traced mode due to `rt_classic_render` guard in `TE_EXPLOSION` handler
- Selecting an upscaler in the video menu left the preset at `Off`; now it defaults to the Quality preset, and `Off` is no longer a selectable preset

---

# vkpt renderer (vendored) — changelog

The RayTracedGL1 renderer is now part of this repository: it is built from source (`vkpt/`) and linked into `vkquake.exe`, so **the game no longer depends on the external `RayTracedGL1.dll` library**. Its changelog is preserved below.

## v3.0.0 (vkpt renderer)

### Added
- **Q2RTX-style core rendering path** — a full new pipeline alongside the legacy one, switched at runtime by the host flag `RG_DEBUG_DRAW_Q2RTX_CORE_BIT` (`rt_core_q2rtx` cvar in vkquake-rt):
  - **ASVGF denoiser** ported from Q2RTX (`asvgf_*.comp`): temporal accumulation, low-frequency (YCoCg luma-SH) and high-frequency/specular atrous filtering, checkerboard interleave
  - **Checkerboard interleave + TAAU** (Q2RTX `taa`) replaces FSR/DLSS upscaling on the new path; FSR 2/3 and DLSS remain available on the legacy path
  - ReSTIR (direct/indirect) remains the lighting solution — `CmQ2Adapter` converts its output into the ASVGF color format (LF_SH / LF_COCG / HF / SPEC)
  - 29 new Q2-format framebuffers (ASVGF colors, history/moments/RNG, TAA history)
- **Fog volumes** — port of Q2RTX `fog.c` + `find_fog_volumes` / `evaluate_fog`:
  - New public API: `rgSetFogVolumes`, `RgFogVolume` (AABB via two diagonal points, color, half-extinction distance, optional soft face), `RG_MAX_FOG_VOLUMES` = 8
  - `CmQ2Fog.comp` blends up to two closest fog volumes along the camera ray in HDR, before tonemapping; works on both the legacy and the new Q2RTX path
- **Q2RTX noise-aware tone mapping** (histogram + curve + apply, Eilertsen et al. + NVIDIA mods)
- **Sun shadow map + god rays** — depth-only world render from the sun's view, ray-marched volumetric light at half-res with a bilateral filter
- **Procedural physical sky** — `RG_SKY_TYPE_PROCEDURAL`: analytic single-scattering atmosphere (Rayleigh + Mie), sun disc, procedural fBm clouds (no textures), HDR cubemap 1024², cached when clouds are off

### Fixed
- **std140 layout of scalar arrays in generated C headers** — GLSL std140 aligns every array element to 16 bytes, but the header generator emitted scalar arrays with a 4-byte stride (e.g. `uint[8]` became `uint32_t[8]` = 32 bytes instead of 128), shifting every field after the first scalar array by 96 bytes on the CPU side. The GPU then read garbage (fog volumes silently never rendered while host-side diagnostics looked correct). The generator now emits `count*4` scalars per std140 array element

## v2.2.0 (vkpt renderer)

### Fixed
- **Denoised ghosting** — `CmSVGFTemporalAccumulation.comp` reworked to remove ghosting artifacts in the SVGF/ASVGF temporal accumulation

## v2.1.0 (vkpt renderer)

### Added
- **AMD FSR 3.1 upscaler** via FidelityFX SDK 1.1.4 (`ffxCreateContext` / `ffxDispatch` / `ffxQuery` API)
- `RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3` — new public enum value for FSR 3.1
- `RG_RENDER_RESOLUTION_MODE_NATIVE_AA` — Native AA mode (render at 1.0x, FSR 3.1 anti-aliasing only)
- AMD-signed prebuilt `amd_fidelityfx_vk.dll` required at runtime (driver overlay detection depends on Authenticode signature)
- **Explicit FSR version selection** — vkpt tells the FidelityFX framework which FSR algorithm to use via `ffxOverrideVersion` instead of letting the framework pick the "best" provider by itself:
  - `RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2` → FSR 2.x
  - `RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3` → FSR 3.1
- Switching between FSR 2 and FSR 3.1 works at runtime (the FidelityFX context is recreated on version change)
- If the requested FSR version is not present in `amd_fidelityfx_vk.dll`, vkpt falls back to the other version and prints a message to the game console (`pfnPrint`)
- `rgIsRenderUpscaleTechniqueAvailable` now actually checks whether the requested FSR version exists in the DLL

### Changed
- FSR 3.1 is always enabled (no compile-time flag; `RG_USE_FSR3` is unconditional)
- `Source/FSR3.{h,cpp}` renamed to `Source/FSR.{h,cpp}`, class `FSR3` renamed to `FSR` (it now handles both FSR 2 and FSR 3.1)

### Removed
- Old FSR2 code (`Source/FSR2.cpp`, `Source/FSR2.h`)
- `RG_WITH_FSR3` CMake option — FSR 3.1 is always built-in

### Fixed
- **AMD RDNA 4 (RX 9070 XT) crash at startup with `VK_ERROR_OUT_OF_DEVICE_MEMORY`** — VMA custom pools (`texturesStagingPool` / `texturesFinalPool`) used a fixed `memoryTypeIndex` that pointed to the wrong memory heap on RDNA 4. Fix: on AMD GPUs, pools are no longer created; `VMA_MEMORY_USAGE_CPU_ONLY` and `VMA_MEMORY_USAGE_GPU_ONLY` are used directly, letting VMA pick the correct heap. Non-AMD GPUs are unaffected
- Missing `VK_KHR_get_memory_requirements2` device extension (caused crash in `ffxCreateContext`)
- DLL digital signature required by AMD driver overlay for FSR detection
