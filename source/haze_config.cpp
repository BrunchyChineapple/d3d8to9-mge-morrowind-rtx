/**
 * haze_config.cpp - runtime in-game tuning for the distant-land aerial-perspective inscatter.
 *
 * Single source of truth for the haze knobs (enabled/start/range/strength/desat) consumed by
 * renderffp.cpp::renderDistantLandFFP. The "MGE Haze" MWSE MCM writes a small "mge_haze.cfg"
 * sidecar to the game root; this unit polls it on a time throttle (~500ms) so the sliders apply
 * live without a restart, exactly like world_batch.cpp.
 *
 * When the cfg is absent (MCM not installed), it falls back to the MGE_HAZE_* environment
 * variables, then to the built-in defaults in HazeConfig.
 */

#ifdef MGE_RTX

#include "haze_config.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace {

HazeConfig g_cfg;
bool       g_haveCfgEver = false;   // once a cfg is read, stop consulting env fallback
ULONGLONG  g_nextPollMs  = 0;
bool       g_initialized = false;

float envF(const char* name, float def) {
    if (const char* e = getenv(name)) {
        if (e[0]) return (float)atof(e);
    }
    return def;
}

// Env/headless fallback used until/unless an mge_haze.cfg appears.
void applyEnvFallback(HazeConfig& c) {
    c.enabled  = true;   // no master env; on unless the MCM cfg disables it
    c.start    = envF("MGE_HAZE_START",    0.0f);
    c.range    = envF("MGE_HAZE_RANGE",    24576.0f);
    c.strength = envF("MGE_HAZE_STRENGTH", 3.0f);
    c.desat    = envF("MGE_HAZE_DESAT",    0.0f);
    if (c.range < 1.0f) c.range = 24576.0f;
}

// Parse "key value" lines from mge_haze.cfg. Returns true if the file was read.
bool readCfg(HazeConfig& c) {
    FILE* f = nullptr;
    if (fopen_s(&f, "mge_haze.cfg", "r") != 0 || !f) return false;
    c = HazeConfig{};   // defaults for any omitted key
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char  key[32] = {};
        float val = 0.0f;
        if (sscanf_s(line, "%31s %f", key, (unsigned)sizeof(key), &val) == 2) {
            if      (_stricmp(key, "enabled")  == 0) c.enabled  = (val != 0.0f);
            else if (_stricmp(key, "start")    == 0) c.start    = val;
            else if (_stricmp(key, "range")    == 0) c.range    = val;
            else if (_stricmp(key, "strength") == 0) c.strength = val;
            else if (_stricmp(key, "desat")    == 0) c.desat    = val;
        }
    }
    fclose(f);
    if (c.range < 1.0f) c.range = 24576.0f;
    return true;
}

void poll() {
    const ULONGLONG now = GetTickCount64();
    if (g_initialized && now < g_nextPollMs) return;
    g_nextPollMs = now + 500;
    g_initialized = true;

    HazeConfig c;
    if (readCfg(c)) {
        g_haveCfgEver = true;
        g_cfg = c;
    } else if (!g_haveCfgEver) {
        // No cfg yet - honor env vars (and keep checking for a cfg each poll).
        applyEnvFallback(g_cfg);
    }
    // If a cfg was seen before but is now missing, keep the last cfg values (don't snap back) -
    // avoids a transient flip if the MCM is mid-rewrite.
}

} // namespace

const HazeConfig& hazeConfig() { poll(); return g_cfg; }

#endif // MGE_RTX
