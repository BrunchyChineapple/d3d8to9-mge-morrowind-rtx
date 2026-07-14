#pragma once

#ifdef MGE_RTX

// Runtime, in-game tuning for MGE distant-static culling (see
// renderffp.cpp::renderDistantStaticsFFP). Two independent tools, one cfg + MCM:
//
//  1. RADIAL NEAR-CULL (enabled/cells): skip distant statics within N cells of the
//     player, so the engine's near static (and any Remix replacement) stands alone.
//     Blanket -- affects every static in the radius.
//
//  2. PER-TEXTURE SUPPRESS (suppress <name> lines): skip distant statics whose SOURCE
//     TEXTURE basename matches a suppress entry. This is the scoped tool: console-click
//     a tree you've replaced, the MCM grabs its texture name, and only THAT tree type's
//     distant copies vanish -- everything else keeps its MGE distant static. Distant
//     statics carry no object id/NIF name at draw time; the texture is the only stable
//     nameable identity (captured at load), so matching is per source texture.
//
// Driven by the "MGE Distant Cull" MWSE MCM, which writes "mge_dlcull.cfg" to the game
// root; polled ~500ms (like haze_config.cpp) so toggles/slider/suppress-list apply LIVE.
//
// mge_dlcull.cfg keys ("key value" per line):
//   enabled  1|0
//   cells    <float, radial cull radius in 8192u cells>
//   suppress <texture-basename-substring>   (repeatable; lowercased; no path/ext needed)
//
// Fallback when the cfg is absent: MGE_DL_NEARCULL_CELLS env (near-cull only), then the
// built-in defaults (disabled, no suppress list).

struct DLCullConfig {
    bool  enabled = false;   // radial near-cull: off => no radial cull
    float cells   = 0.0f;    // radial near-cull radius, in 8192u cells (0 => none)
};

// Polls mge_dlcull.cfg (~500ms) with env fallback. Call once per distant-statics frame
// (this is what refreshes BOTH the near-cull knobs and the suppress list/state below).
const DLCullConfig& dlCullConfig();

// --- Per-texture distant-static suppression -----------------------------------------
// Register a distant static's source texture + its file path at load time. The name is
// copied (lowercased); `texPtr` is the IDirect3DTexture9* that will appear as RenderMesh.tex
// at render time. Safe to call repeatedly. No-op for null.
void dlRegisterDistantTexture(const void* texPtr, const char* texName);

// True if the distant static using this texture should be skipped (its texture path
// contains any current `suppress` entry). O(1) after the first lookup per texture; the
// per-texture verdict is cached and invalidated automatically when the suppress list
// changes. Does not poll -- call dlCullConfig() once per frame first.
bool dlIsDistantTextureSuppressed(const void* texPtr);

#endif // MGE_RTX
