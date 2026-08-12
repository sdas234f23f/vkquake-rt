# Changelog

## v2.1.0

### Added
- **AMD FSR 3.1 upscaler** support (`rt_upscale_fsr31` cvar) with quality presets: Native AA, Quality, Balanced, Performance, Ultra Performance
- **RT dynamic lights for explosions** — rocket/grenade explosions now emit spherical lights in ray-traced mode (previously only worked in classic renderer)
- **Redesigned video menu**: upscalers (FSR 2.0, FSR 3.1, DLSS) grouped under a single `Upscaler` selector with a separate `Quality` preset option. Only available upscalers are shown for the current GPU

### Fixed
- RT explosion lights were missing in ray-traced mode due to `rt_classic_render` guard in `TE_EXPLOSION` handler

