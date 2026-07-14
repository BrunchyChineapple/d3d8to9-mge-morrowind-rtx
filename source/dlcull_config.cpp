/**
 * dlcull_config.cpp - runtime in-game tuning for MGE distant-static culling.
 *
 * Two tools sharing one polled sidecar (mge_dlcull.cfg, written by the "MGE Distant Cull"
 * MWSE MCM, ~500ms throttle, exactly like haze_config.cpp):
 *   1. radial near-cull  (enabled/cells)                -> DLCullConfig / dlCullConfig()
 *   2. per-texture suppress (repeatable `suppress` keys) -> dlRegisterDistantTexture /
 *                                                           dlIsDistantTextureSuppressed
 *
 * The suppress list is matched against each distant static's SOURCE TEXTURE path (the only
 * stable nameable identity distant statics carry -- they have no object id/NIF name at draw
 * time). A `suppress <name>` entry matches when the texture path CONTAINS <name> (case-
 * insensitive substring), so the MCM can write a bare basename (no directory / extension)
 * and still match "textures\\foo.dds".
 *
 * Absent cfg -> MGE_DL_NEARCULL_CELLS env fallback (near-cull only), then defaults (off).
 */

#ifdef MGE_RTX

#include "dlcull_config.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>

namespace {

DLCullConfig g_cfg;
bool         g_haveCfgEver = false;   // once a cfg is read, stop consulting env fallback
ULONGLONG    g_nextPollMs  = 0;
bool         g_initialized = false;

// Suppress list (lowercased substrings) + a generation bumped whenever it changes.
std::vector<std::string> g_suppress;
unsigned                 g_suppressGen = 0;

// tex* -> lowercased texture path, filled at distant-static load.
std::unordered_map<const void*, std::string> g_texNames;

// Per-texture suppress verdict cache, tagged with the suppress generation it was built for.
std::unordered_map<const void*, char> g_suppressedCache;
unsigned                              g_cacheGen = (unsigned)-1;

std::string toLower(const char* s) {
    std::string out(s ? s : "");
    for (char& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

// Env/headless fallback (near-cull radius only; no env suppress channel).
void applyEnvFallback(DLCullConfig& c) {
    c.cells = 0.0f;
    if (const char* e = getenv("MGE_DL_NEARCULL_CELLS")) {
        if (e[0]) c.cells = (float)atof(e);
    }
    if (c.cells < 0.0f) c.cells = 0.0f;
    c.enabled = (c.cells > 0.0f);
}

// Parse mge_dlcull.cfg. Returns true if the file was read. Fills near-cull cfg + suppress.
bool readCfg(DLCullConfig& c, std::vector<std::string>& suppress) {
    FILE* f = nullptr;
    if (fopen_s(&f, "mge_dlcull.cfg", "r") != 0 || !f) return false;
    c = DLCullConfig{};
    suppress.clear();
    char line[260];
    while (fgets(line, sizeof(line), f)) {
        char key[32] = {};
        char rest[240] = {};
        // key + remainder of line (remainder may contain spaces for paths)
        if (sscanf_s(line, "%31s %239[^\r\n]", key, (unsigned)sizeof(key), rest, (unsigned)sizeof(rest)) >= 1) {
            if (_stricmp(key, "enabled") == 0) {
                c.enabled = (atof(rest) != 0.0);
            } else if (_stricmp(key, "cells") == 0) {
                c.cells = (float)atof(rest);
            } else if (_stricmp(key, "suppress") == 0) {
                // trim leading/trailing whitespace of rest
                char* s = rest;
                while (*s == ' ' || *s == '\t') ++s;
                int end = (int)strlen(s);
                while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) s[--end] = '\0';
                if (end > 0) suppress.push_back(toLower(s));
            }
        }
    }
    fclose(f);
    if (c.cells < 0.0f) c.cells = 0.0f;
    return true;
}

bool suppressListsDiffer(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.size() != b.size()) return true;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return true;
    return false;
}

void poll() {
    const ULONGLONG now = GetTickCount64();
    if (g_initialized && now < g_nextPollMs) return;
    g_nextPollMs = now + 500;
    g_initialized = true;

    DLCullConfig c;
    std::vector<std::string> suppress;
    if (readCfg(c, suppress)) {
        g_haveCfgEver = true;
        g_cfg = c;
        if (suppressListsDiffer(g_suppress, suppress)) {
            g_suppress.swap(suppress);
            ++g_suppressGen;   // invalidates the per-texture verdict cache lazily
        }
    } else if (!g_haveCfgEver) {
        applyEnvFallback(g_cfg);
        if (!g_suppress.empty()) { g_suppress.clear(); ++g_suppressGen; }
    }
    // cfg vanished after being seen -> keep last values (avoid mid-rewrite flip).
}

} // namespace

const DLCullConfig& dlCullConfig() { poll(); return g_cfg; }

void dlRegisterDistantTexture(const void* texPtr, const char* texName) {
    if (!texPtr || !texName) return;
    g_texNames[texPtr] = toLower(texName);
    // A freshly registered texture is not yet in the verdict cache; it will be computed
    // on first query. No generation bump needed (the suppress LIST didn't change).
}

bool dlIsDistantTextureSuppressed(const void* texPtr) {
    if (g_suppress.empty() || !texPtr) return false;

    if (g_cacheGen != g_suppressGen) {   // suppress list changed -> drop stale verdicts
        g_suppressedCache.clear();
        g_cacheGen = g_suppressGen;
    }
    auto cit = g_suppressedCache.find(texPtr);
    if (cit != g_suppressedCache.end()) return cit->second != 0;

    char verdict = 0;
    auto nit = g_texNames.find(texPtr);
    if (nit != g_texNames.end()) {
        const std::string& name = nit->second;
        for (const std::string& s : g_suppress) {
            if (!s.empty() && name.find(s) != std::string::npos) { verdict = 1; break; }
        }
    }
    g_suppressedCache[texPtr] = verdict;
    return verdict != 0;
}

#endif // MGE_RTX
