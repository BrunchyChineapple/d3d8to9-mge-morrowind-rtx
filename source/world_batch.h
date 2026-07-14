#pragma once

#ifdef MGE_RTX

// Runtime, in-game toggles for the static-batching system. Driven by the "MGE Batching" MWSE
// MCM (mods/mge_batch), which writes a single sidecar "mge_batch.cfg" to the game root. The
// wrapper POLLS that file on a throttle (~1s, like rt_anticull), so the MCM master switch and
// the per-tier sub-toggles take effect live, WITHOUT editing files or restarting the game.
//
// Config keys in mge_batch.cfg ("key value" per line, 0/1):
//   master   — master switch for the WHOLE batching system
//   distant  — Tier 1 sub-toggle (distant statics, renderffp.cpp::renderDistantStaticsFFP)
//   near     — Tier 4 sub-toggle (near statics, d3d8to9_device.cpp::worldBatchSubmit)
//
// Effective gates (what the wrapper actually checks):
//   worldBatchTier1Enabled() == master && distant
//   worldBatchTier4Enabled() == master && near
//
// Backward compatibility: when mge_batch.cfg is ABSENT (no MCM installed), the toggles fall
// back to the legacy marker files so existing deployments keep working — master defaults ON,
// distant follows "mge_batch_statics.txt" (or env MGE_BATCH_STATICS), near follows
// "mge_batch_world_apply.txt" (or env MGE_BATCH_WORLD_APPLY). Once the MCM writes the cfg, it
// is the single source of truth and the markers are ignored.
//
// Defined in world_batch.cpp (compiled once into d3d8.dll) so the single shared poll state is
// consulted identically by both translation units (Tier 1 in the MGE-XE submodule, Tier 4 in
// the wrapper core).

bool worldBatchTier1Enabled();   // distant statics (Tier 1)
bool worldBatchTier4Enabled();   // near statics (Tier 4)

#endif // MGE_RTX
