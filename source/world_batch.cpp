/**
 * world_batch.cpp — runtime in-game toggles for the static-batching system.
 *
 * Single source of truth for the master + per-tier enable state, consulted by both batching
 * tiers (Tier 1 distant statics in renderffp.cpp, Tier 4 near statics in d3d8to9_device.cpp).
 * The "MGE Batching" MWSE MCM (mods/mge_batch) writes a small "mge_batch.cfg" sidecar to the
 * game root; this unit polls it on a time throttle (~500ms) so the in-game toggles apply live
 * without a restart, exactly like the rt_anticull control plane.
 *
 * When the cfg is absent (MCM not installed), it falls back to the legacy marker files so
 * existing deployments keep working: master ON, distant <- mge_batch_statics.txt / env
 * MGE_BATCH_STATICS, near <- mge_batch_world_apply.txt / env MGE_BATCH_WORLD_APPLY.
 */

#ifdef MGE_RTX

#include "world_batch.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace {

struct BatchToggles {
	bool master  = true;
	bool distant = false;   // Tier 1
	bool near_   = false;   // Tier 4
};

BatchToggles g_tog;
bool         g_haveCfgEver = false;   // once we have read a cfg, stop consulting legacy markers
ULONGLONG    g_nextPollMs  = 0;
bool         g_initialized = false;

bool markerOn(const char* file, const char* env) {
	if (const char* e = getenv(env)) {
		if (e[0] && e[0] != '0') return true;
	}
	return GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES;
}

// Legacy fallback used until/unless an mge_batch.cfg appears.
void applyLegacyMarkers(BatchToggles& t) {
	t.master  = true;
	t.distant = markerOn("mge_batch_statics.txt", "MGE_BATCH_STATICS");
	t.near_   = markerOn("mge_batch_world_apply.txt", "MGE_BATCH_WORLD_APPLY");
}

// Parse "key value" lines (0/1) from mge_batch.cfg. Returns true if the file was read.
bool readCfg(BatchToggles& t) {
	FILE* f = nullptr;
	if (fopen_s(&f, "mge_batch.cfg", "r") != 0 || !f) return false;
	// Defaults when a key is omitted: master ON, sub-toggles OFF (the MCM always writes all 3).
	t.master = true; t.distant = false; t.near_ = false;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		char key[32] = {};
		int val = 0;
		if (sscanf_s(line, "%31s %d", key, (unsigned)sizeof(key), &val) == 2) {
			if      (_stricmp(key, "master")  == 0) t.master  = (val != 0);
			else if (_stricmp(key, "distant") == 0) t.distant = (val != 0);
			else if (_stricmp(key, "near")    == 0) t.near_   = (val != 0);
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

	BatchToggles t;
	if (readCfg(t)) {
		g_haveCfgEver = true;
		g_tog = t;
	} else if (!g_haveCfgEver) {
		// No cfg yet — honor legacy markers (and keep checking for a cfg each poll).
		applyLegacyMarkers(g_tog);
	}
	// If a cfg was seen before but is now missing, keep the last cfg values (don't snap back to
	// markers) — avoids a transient flip if the MCM is mid-rewrite.
}

} // namespace

bool worldBatchTier1Enabled() { poll(); return g_tog.master && g_tog.distant; }
bool worldBatchTier4Enabled() { poll(); return g_tog.master && g_tog.near_; }

#endif // MGE_RTX
