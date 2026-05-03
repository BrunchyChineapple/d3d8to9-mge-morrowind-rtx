# d3d8to9-mge — MGE-XE + d3d8to9 for RTX Remix

This is based on rfuzzo's earlier experiments with creating a experimental port of MGE-XE onto d3d8to9
Unified d3d8→d3d9 translation layer with MGE-XE distant land support for Morrowind + RTX Remix.

## Architecture

```
Morrowind.exe (D3D8)
    → d3d8.dll (this project)
        → MGEProxyDevice (inherits d3d8to9's Direct3DDevice8)
            → Intercepts draw calls for distant land injection
            → Distant statics/land rendered via FFP pipeline (Remix-compatible)
            → Remix SDK API used for fog sync and physical sky
        → IDirect3DDevice9 (real D3D9 → RTX Remix Bridge → d3d9.dll)
```

## Components

- **d3d8to9** (source/) — crosire's D3D8→D3D9 translation layer
- **MGE-XE 0.18.0** (MGE-XE/) — [Hrnchamd's official MGE-XE](https://github.com/Hrnchamd/MGE-XE) (git submodule)
- **MGEProxyDevice** (MGE-XE/src/mge/mged3d8device.cpp) — Bridge class connecting d3d8to9 and MGE-XE
- **Remix SDK** (source/remix_c.h, source/remix_api_test.cpp) — Remix API interface for runtime config

## Build Requirements

- Visual Studio 2022 (v143 toolset)
- DirectX SDK (June 2010) — for d3dx9 headers and libs
- Windows 10 SDK
- Target: Win32 (x86)

## Build Defines

| Define | Purpose |
|--------|---------|
| `MGE_XE` | Enables MGE-XE integration in d3d8to9 source |
| `MGE_RTX` | Enables RTX Remix specific behavior (FFP rendering, fog/sky sync) |
| `D3DX9` | Links d3dx9 directly instead of runtime loading |
| `D3D8TO9NOLOG` | Disables d3d8to9 verbose logging (Release only) |
| `NOMINMAX` | Prevents Windows.h min/max macro conflicts |

## RTX Remix Integration

### Rendering Path (renderffp.cpp)

MGE-XE's distant land normally renders through D3DX effect shaders, which Remix cannot
interpret. The `MGE_RTX` build replaces these with a D3D9 fixed-function pipeline path
that Remix understands:

- **Distant statics** — Compressed vertex buffers (FLOAT16_4 position, UBYTE4N normal,
  D3DCOLOR diffuse, FLOAT16_2 texcoord) are decompressed to standard FVF format and
  rendered with `DrawIndexedPrimitive`
- **Distant land** — Terrain chunks (FLOAT3 position, SHORT2N texcoord) are decompressed
  and rendered with the world colour map texture
- **TTL retention** — Meshes leaving the view frustum are kept for 3 frames to prevent
  pop-out flicker with Remix's temporal denoiser

### Fog Sync (distantland.cpp — syncRemixFog)

MGE-XE's proxy intercepts Morrowind's D3D9 fog render states (`FOGSTART`, `FOGEND`,
`FOGVERTEXMODE`, `FOGTABLEMODE`) and handles fog in its own shaders. This prevents
Remix's volumetric fog system from reading the fog state. The fog sync forces the
computed fog parameters onto the D3D9 device each frame after `adjustFog()` runs.

Volumetric fog density and color are controlled via `rtx.conf` / `user.conf`:
- `rtx.volumetrics.transmittanceMeasurementDistanceMeters`
- `rtx.volumetrics.transmittanceColor`
- `rtx.volumetrics.singleScatteringAlbedo`

### Physical Sky Sync (distantland.cpp — syncRemixSky)

Drives Remix's Hillaire physically-based atmospheric sky (`rtx.skyMode = 1`) from
Morrowind's sun position. Reads the sun direction from `MWBridge::GetSunDir()` each
frame and converts to elevation/rotation angles via `SetConfigVariable`:

- `rtx.atmosphere.sunElevation` — `asin(sunDir.z)` in degrees
- `rtx.atmosphere.sunRotation` — `atan2(sunDir.x, sunDir.y)` in degrees

Morrowind's scenegraph bounces the sun direction at the horizon (never returns negative Z),
so game hour is used to detect night and negate the elevation for proper sunset/sunrise
transitions.

### Remix SDK API (source/remix_api_test.cpp)

Initializes the Remix SDK from the 32-bit bridge client via `remixapi_lib_loadRemixDllAndInitialize`.
Requires `exposeRemixApi = True` in `.trex/bridge.conf`. The `REMIX_ALLOW_X86` define
bypasses the 64-bit check since the bridge forwards API calls to the 64-bit server.

Confirmed working API functions: `CreateMesh`, `DestroyMesh`, `CreateMaterial`,
`DestroyMaterial`, `DrawInstance`, `CreateLight`, `DrawLightInstance`, `SetConfigVariable`.
`SetupCamera` is null (handled internally by the bridge).

### RTX Compatibility Flags (d3d8to9_base.cpp)

Set at device creation to disable MGE-XE features that conflict with Remix:

| Flag | Effect |
|------|--------|
| `NO_MW_MGE_BLEND` | Disables the blend pass that composites distant land via screen-space blit |
| `~USE_HW_SHADER` | Disables post-process shaders that conflict with Remix's denoiser |
| `~USE_MENU_CACHING` | Disables cached render blits that Remix can't track temporally |

## Runtime Requirements

- RTX Remix Bridge (`.trex/` directory with `NvRemixBridge.exe`, `d3d9.dll`, `bridge.conf`)
- `exposeRemixApi = True` in `.trex/bridge.conf`
- `rtx.skyMode = 1` in `rtx.conf` for physical atmosphere sky
- `rtx.zUp = True` in `rtx.conf` (Morrowind uses Z-up coordinates)

## Known Limitations

- Distant land textures are extracted by Remix from FFP draw calls, not via SDK materials.
  The Remix SDK's file-path-based material system does not work through the 32→64 bit bridge
  (texture paths don't resolve on the server side).
- Moons and stars are not visible with the physical atmosphere sky (`skyMode = 1`) since
  Remix skips all sky-categorized geometry in this mode.
- Fog parameters are static in `rtx.conf` — dynamic weather-based fog changes from MGE-XE
  are not yet synced to Remix's volumetric system at runtime.

## Credits

- [crosire/d3d8to9](https://github.com/crosire/d3d8to9) — D3D8 to D3D9 wrapper
- [Hrnchamd/MGE-XE](https://github.com/Hrnchamd/MGE-XE) — MGE-XE 0.18.0 (official)
- [rfuzzo/d3d8to9](https://github.com/rfuzzo/d3d8to9) — Original MGE-XE + d3d8to9 integration concept
- [NVIDIA RTX Remix](https://github.com/NVIDIAGameWorks/dxvk-remix) — Ray tracing runtime
