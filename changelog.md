# Changelog

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
  - `Tools/convert_ovrd_mat.py` — converts RTGL1 override materials (`ovrd/mat/*.ktx2`, RGBA8/BC5/BC7) into a ready-to-use `.pkz` (textures + `.mat`)
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
- **Q2RTX core path switch** — `rt_core_q2rtx` cvar enables the new Q2RTX-style rendering core in RTGL1 (ASVGF denoiser + checkerboard interleave + TAAU)
- **Fog volumes** — port of the Q2RTX `fog` console command: define up to 8 axis-aligned fog boxes with `fog -v <i> -a <x,y,z|here> -b ... -c <r,g,b> -d <dist> -f <face>`; print (`-p`), reset (`-r`) and clear-all (`-R`) supported
- **Sun presets** — `rt_sun_preset` cvar (manual, warm, daylight, neutral, sunset, cold, Q1 purple, cold blue); sun color now lives in separate `rt_sky_light_r/g/b` cvars, decoupled from `rt_globallight_*`
- **Physical sky support** — `rt_physical_sky` enables the procedural atmosphere in RTGL1; `rt_sky_tint` and cloud cvars control the look
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
