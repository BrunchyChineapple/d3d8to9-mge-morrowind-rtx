/**
 * sky_config.cpp - runtime in-game master toggles for the wrapper-driven RTX sky systems.
 *
 * The "MGE Sky" MWSE MCM writes a small "mge_sky.cfg" sidecar to the game root; this unit polls it
 * on a time throttle (~500ms) so the toggles apply live without a restart. syncRemixSky() reads
 * skyConfig() each frame and gates its Derived-layer writes accordingly, so the toggles are not
 * stomped (the wrapper is the thing doing the stomping, and it now honors the user's choice).
 *
 * When the cfg is absent (MCM not installed), it falls back to the MGE_* environment variables,
 * then to the built-in defaults in SkyConfig.
 */

#ifdef MGE_RTX

#include "sky_config.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace {

SkyConfig g_cfg;
bool      g_haveCfgEver = false;
ULONGLONG g_nextPollMs  = 0;
bool      g_initialized = false;

bool envBool(const char* name, bool def) {
    if (const char* e = getenv(name)) {
        if (e[0]) return (e[0] != '0');
    }
    return def;
}

void applyEnvFallback(SkyConfig& c) {
    c.distantFog     = envBool("MGE_DISTANT_FOG",   true);
    c.constellations = envBool("MGE_CONSTELLATIONS", false);
    c.meteors        = envBool("MGE_METEORS",        true);
}

bool readCfg(SkyConfig& c) {
    FILE* f = nullptr;
    if (fopen_s(&f, "mge_sky.cfg", "r") != 0 || !f) return false;
    c = SkyConfig{};   // defaults for any omitted key
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[32] = {};
        int  val = 0;
        if (sscanf_s(line, "%31s %d", key, (unsigned)sizeof(key), &val) == 2) {
            if      (_stricmp(key, "distantFog")     == 0) c.distantFog     = (val != 0);
            else if (_stricmp(key, "constellations") == 0) c.constellations = (val != 0);
            else if (_stricmp(key, "meteors")        == 0) c.meteors        = (val != 0);
        }
    }
    fclose(f);
    return true;
}

void poll() {
    const ULONGLONG now = GetTickCount64();
    if (g_initialized && now < g_nextPollMs) return;
    g_nextPollMs = now + 500;
    g_initialized = true;

    SkyConfig c;
    if (readCfg(c)) {
        g_haveCfgEver = true;
        g_cfg = c;
    } else if (!g_haveCfgEver) {
        applyEnvFallback(g_cfg);
    }
    // If a cfg was seen before but is now missing, keep the last values (avoid transient flip).
}

} // namespace

const SkyConfig& skyConfig() { poll(); return g_cfg; }

#endif // MGE_RTX
