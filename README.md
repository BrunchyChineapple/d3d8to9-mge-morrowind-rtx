# d3d8to9-mge — MGE-XE UF + d3d8to9 for RTX Remix

Unified d3d8→d3d9 translation layer with MGE-XE distant land support for Morrowind + RTX Remix.

## Architecture

```
Morrowind.exe (D3D8)
    → d3d8.dll (this project)
        → MGEProxyDevice (inherits d3d8to9's Direct3DDevice8)
            → Intercepts draw calls for distant land injection
            → DistantLand renders via ID3DXEffect shaders
        → IDirect3DDevice9 (real D3D9 → RTX Remix d3d9.dll)
```

## Components

- **d3d8to9** (source/) — crosire's D3D8→D3D9 translation layer (upstream, with minimal modifications)
- **MGE-XE UF** (MGE-XE/) — NullCascade's fork with 64-bit distant land server (git submodule)
- **MGEProxyDevice** (source/mge/) — Bridge class connecting d3d8to9 and MGE-XE

## Build Requirements

- Visual Studio 2022 (v143 toolset)
- DirectX SDK (June 2010) — for d3dx9 headers and libs
- Windows 10 SDK
- Target: Win32 (x86)

## Build Defines

| Define | Purpose |
|--------|---------|
| `MGE_XE` | Enables MGE-XE integration in d3d8to9 source |
| `MGE_RTX` | Enables RTX Remix specific behavior |
| `D3DX9` | Links d3dx9 directly instead of runtime loading |
| `D3D8TO9NOLOG` | Disables d3d8to9 verbose logging (Release only) |

## Key Fixes Over rfuzzo's Implementation

1. **Proper depth bias** — Retains upstream's bit-count-aware Z-bias calculation
2. **Correct render target detection** — Compares against backbuffer instead of blanket disable
3. **Vertex declaration cleanup** — Keeps `SetVertexDeclaration(nullptr)` on FVF switch
4. **Water node marking** — Re-enabled for proper water replacement
5. **IPC support** — Includes NullCascade's 64-bit distant land server communication

## Credits

- [crosire/d3d8to9](https://github.com/crosire/d3d8to9) — D3D8 to D3D9 wrapper
- [NullCascade/MGE-XE](https://github.com/NullCascade/MGE-XE) — MGE-XE Unofficial Fork
- [rfuzzo/d3d8to9](https://github.com/rfuzzo/d3d8to9) — Original MGE-XE + d3d8to9 integration concept
