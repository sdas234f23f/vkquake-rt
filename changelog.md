# Changelog

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
