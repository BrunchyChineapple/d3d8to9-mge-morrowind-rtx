#pragma once

#ifdef MGE_RTX

// Runtime, in-game tuning for the distant-land aerial-perspective inscatter (Route 1, baked into
// the FFP land draw's TFACTOR via D3DTOP_ADD; see renderffp.cpp::renderDistantLandFFP). Driven by
// the "MGE Haze" MWSE MCM, which writes a small "mge_haze.cfg" sidecar to the game root. The
// wrapper POLLS that file on a throttle (~500ms, like world_batch / rt_anticull), so the MCM
// sliders take effect LIVE, without editing files or restarting the game.
//
// Config keys in mge_haze.cfg ("key value" per line):
//   enabled  — master on/off (1/0)
//   start    — world units where haze begins (0 => nearViewRange, the distant-land start)
//   range    — ramp distance over which haze builds (world units; default 24576 = 3 cells)
//   strength — final inscatter multiplier (default 3.0)
//   desat    — 0..1, pulls the haze toward neutral grey (tames the saturated clear-day blue)
//
// Backward/headless compatibility: when mge_haze.cfg is ABSENT (no MCM installed), the values
// fall back to the MGE_HAZE_* environment variables (MGE_HAZE_START/RANGE/STRENGTH/DESAT), and
// failing those, to the built-in defaults below. Once the MCM writes the cfg it is the single
// source of truth.
//
// Implemented in haze_config.cpp (compiled once into d3d8.dll).

struct HazeConfig {
    bool  enabled  = true;
    float start    = 0.0f;        // 0 => nearViewRange
    float range    = 24576.0f;    // 3 cells
    float strength = 3.0f;
    float desat    = 0.0f;        // 0..1
};

// Polls mge_haze.cfg (~500ms) with env-var fallback. Returns a reference to shared poll state;
// values are stable within a frame. Call once per land-draw frame.
const HazeConfig& hazeConfig();

#endif // MGE_RTX
