#pragma once

#ifdef MGE_RTX

// Runtime, in-game master toggles for the wrapper-driven RTX sky/atmosphere systems. Several of
// these options are NoSave and written to the Derived layer by syncRemixSky() every frame, which
// means the Remix UI checkboxes get instantly stomped. To give the user real control, the WRAPPER
// itself honors these toggles when deciding what to drive. Driven by the "MGE Sky" MWSE MCM, which
// writes a small "mge_sky.cfg" sidecar to the game root; the wrapper polls it on a throttle
// (~500ms, like world_batch / haze_config), so the toggles apply LIVE without a restart.
//
// Config keys in mge_sky.cfg ("key value" per line, 0/1):
//   distantFog     - master for the EXTERIOR volumetric fog (rtx.volumetrics.enable). When off the
//                    wrapper writes enable=False outdoors (Remix falls back to cheap depth fog).
//   constellations - constellation overlay (rtx.atmosphere.constellationsEnabled)
//   meteors        - meteor / shower system (rtx.atmosphere.meteorsEnabled)
//
// Interiors always force these OFF regardless of the toggles (existing sky-gating behavior); the
// toggles only affect the exterior-return writes.
//
// Fallback when mge_sky.cfg is ABSENT: the MGE_* environment variables (MGE_DISTANT_FOG,
// MGE_CONSTELLATIONS, MGE_METEORS), then the built-in defaults below.
//
// Implemented in sky_config.cpp (compiled once into d3d8.dll).

struct SkyConfig {
    bool distantFog     = true;    // exterior volumetric fog master
    bool constellations = false;   // constellation overlay (off until the signs are tuned)
    bool meteors        = true;    // meteor / shower system
};

// Polls mge_sky.cfg (~500ms) with env-var fallback. Call once per frame.
const SkyConfig& skyConfig();

#endif // MGE_RTX
