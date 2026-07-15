#pragma once

#ifdef MGE_RTX

// RT_AntiCull — ray-tracing-aware anti-culling for Morrowind under RTX Remix.
//
// Native C++ port of the validated decision logic in rt_anticull/ (Python executable
// spec; property tests P1-P18 are the conformance oracle). This module replaces
// Morrowind's per-node scene-graph cull, FrustumCull @ 0x6EB480, so that off-screen
// geometry which still contributes to reflections / shadows / GI stays submitted to
// Remix, while genuinely irrelevant distant geometry is still culled.
//
// Design reference: .kiro/specs/rt-aware-anticull/{requirements,design}.md.
// Engine facts (triple-confirmed): patches/Morrowind/{findings.md,kb.h}.
//
// The hook is a FULL REPLACEMENT of FrustumCull (the same interception the shipping
// v6/v7 AntiCull mod uses), not a call-through detour. Replacement is what lets the
// per-node decision reproduce the formal Kept_Set union
//   K = F  union  {NEAR}  union  {CONDITIONAL and reachable}
// faithfully: a node that the native frustum would keep (set F) renders every frame
// with no cadence/budget gating, while only off-screen CONDITIONAL nodes are paced.

#include <cstdint>

namespace RTAntiCull {

// ---------------------------------------------------------------------------
// Verified engine addresses (Morrowind.exe, preferred base 0x400000, no ASLR).
// Source: patches/Morrowind/kb.h + findings.md (live trace + v6 mod patch bytes +
// MWSE RE'd NetImmerse headers, all in agreement).
// ---------------------------------------------------------------------------
namespace Engine {
    constexpr uintptr_t kFrustumCull   = 0x6EB480;  // __thiscall(NiNode* ecx, NiCullingContext* [esp+4]); ret 4
    constexpr uintptr_t kPlaneTest     = 0x6F2660;  // __thiscall(NiBound* ecx, plane* [esp+4]); 0=isect 1=inside 2=outside
    constexpr uintptr_t kWorldController = 0x7C67DC; // WorldController** global

    // NI::AVObject vtable slots (from MWSE NIAVObject.h, static_assert'd):
    constexpr int kVtbl_getAppCulled = 0x54;  // bool(__thiscall*)(const AVObject*)
    constexpr int kVtbl_display      = 0x78;  // void(__thiscall*)(AVObject*, NiCamera*) — the KEEP/recurse action

    // NI node header field offsets (NI::Object/ObjectNET/AVObject, MWSE headers; bound
    // + name additionally live-confirmed 2026-05-29):
    constexpr int kNode_name         = 0x08;  // char* ObjectNET::name (live-confirmed item C)
    constexpr int kNode_boundCenter  = 0x1C;  // Vector3 worldBoundOrigin
    constexpr int kNode_boundRadius  = 0x28;  // float   worldBoundRadius

    // NiCullingContext (== NiCamera) offsets (live disasm of FrustumCull):
    constexpr int kCtx_cameraPos     = 0x1C;  // Vector3 camera world position (x,y,z at +0x1C/+0x20/+0x24)
    constexpr int kCtx_planeArray    = 0x14C; // pointer to array of frustum-plane pointers
    constexpr int kCtx_planeCount    = 0x160; // int: number of frustum planes (6)

    // NiBound passed to PlaneTest is node+0x1C: {center.xyz at +0x0, radius at +0xC} —
    // contiguous because worldBoundRadius lives at node+0x28 == (node+0x1C)+0xC.
    constexpr int kNode_bound        = 0x1C;

    // WorldController-relative: the 1st-person arm camera pointer (WC+0x160).
    constexpr int kWC_armCamera      = 0x160;
}

// ---------------------------------------------------------------------------
// Geometry + tiers (mirror rt_anticull/models.py).
// ---------------------------------------------------------------------------
struct Vec3 {
    float x, y, z;
};

enum class TierKind {
    Near,        // always keep (R3.2)
    Conditional, // keep while reachable (R3.3)
    Far,         // cull (R3.5)
};

enum class CullMode {
    RtAntiCull,          // full tiered union (default, R7.1)
    AntiCullHeuristic,   // v6/v7 single-plane squared-distance fallback
    NativeFrustumCulling // K == F passthrough (R7.3)
};

// Documented bounds (rt_anticull/models.py — single source of truth there; mirrored).
namespace Bounds {
    constexpr float FrameTimeBudgetMinMs     = 0.5f;
    constexpr float FrameTimeBudgetMaxMs     = 20.0f;
    constexpr float FrameTimeBudgetDefaultMs = 2.0f;

    constexpr int McmReachabilityRangeMin     = 256;
    constexpr int McmReachabilityRangeMax     = 32768;
    constexpr int McmReachabilityRangeDefault = 8192;

    // Per-cell-type range defaults (auto-selected by CellHasWeather). User-tuned sweet
    // spots: exteriors + behaves-as-exterior at 2048, true interiors need >= 4096 to avoid
    // missing geometry. Both are MCM-adjustable within [McmReachabilityRangeMin, Max].
    constexpr int ExteriorRangeDefault = 2048;
    constexpr int InteriorRangeDefault = 4096;

    constexpr int TierRangeMin     = 256;
    constexpr int TierRangeMax     = 131072;
    constexpr int TierRangeDefault = 8192;

    // The NEAR (always-keep) shell is the inner half of the reachability range, capped at
    // this width so high ranges keep a modest always-keep shell while low ranges scale all
    // the way down. At the default range 8192 this yields exactly the legacy NEAR [0,4096).
    constexpr int NearShellMaxWidth = 4096;

    constexpr int DefaultRetentionWindow = 1; // NVIDIA defaults; no Remix getter; R6.5

    constexpr int NearResubmitInterval           = 1;
    constexpr int MinConditionalResubmitInterval = 2;
}

struct SubmissionTier {
    int      id;
    TierKind kind;
    float    nearDist;        // band lower bound, inclusive (world units)
    float    farDist;         // band upper bound, exclusive (FLT_MAX for FAR)
    int      range;           // RT_Reachability_Range, clamped [4096, 131072] (R4.4)
    int      resubmitInterval; // configured cadence; effective interval is min(value, N)
};

constexpr int kMaxTiers = 8;

struct Config {
    CullMode mode             = CullMode::RtAntiCull;
    float    frameTimeBudgetMs = Bounds::FrameTimeBudgetDefaultMs;

    // Two reachability ranges auto-selected by cell type (R5.1/R5.4). Exterior +
    // behaves-as-exterior cells use exteriorRange; true interiors use interiorRange. The
    // wrapper re-selects per frame from MWBridge::CellHasWeather() and feeds the active
    // value into the tier bands. Defaults are the user's tuned sweet spots (2048 / 4096).
    int      exteriorRange    = Bounds::ExteriorRangeDefault;
    int      interiorRange    = Bounds::InteriorRangeDefault;

    // The range currently driving the tier bands (== exteriorRange or interiorRange for the
    // active cell). Updated by selectRangeForCell(); read by the cull path.
    int      activeRange      = Bounds::ExteriorRangeDefault;

    int      tierCount        = 3;
    SubmissionTier tiers[kMaxTiers];

    Config(); // builds default tiers from the default active range
};

// ---------------------------------------------------------------------------
// Lifecycle (called from the wrapper).
// ---------------------------------------------------------------------------

// Install the FrustumCull replacement and read engine globals. Wrapped so it cannot
// throw or crash the render loop: on any failure it returns false and leaves the engine
// unmodified, and the orchestrator falls back to the heuristic mode (R7.4/R7.5).
// Idempotent.
bool initialize();

// True once the hook is installed and active.
bool isInitialized();

// Advance the per-frame counter that drives the resubmission cadence. Call once per
// rendered frame at the frame boundary (Present). Also polls the MWSE-Lua config sidecar
// on a throttled cadence so MCM changes take effect within ~1s without per-frame file I/O.
void beginFrame();

// Remove the hook (restore original bytes). Safe to call if not installed.
void shutdown();

// ---------------------------------------------------------------------------
// MWSE-Lua control plane (item E). The MCM (Lua) writes a key=value sidecar; the wrapper
// polls + parses it and applies validated values to the live config. File-based because it
// decouples the two in-process modules — no shared memory address or ABI to keep in lockstep
// across MWSE/wrapper versions (MWSE-Lua also has no supported native-call/memory-write API).
// ---------------------------------------------------------------------------

// Sidecar the MCM writes and the wrapper reads. Lives next to the game exe.
// Format: one `key value` per line; keys: mode (0|1|2), range (int), enabled (0|1).
constexpr const char* kConfigFileName = "rt_anticull.cfg";

// Mode <-> integer mapping shared with the Lua MCM dropdown (R7.1).
//   0 = RT_AntiCull (default), 1 = AntiCull_Heuristic, 2 = Native_Frustum_Culling.
int      modeToInt(CullMode mode);
CullMode modeFromInt(int value);

// Apply a validated mode to the live config (R5.6). Plain scalar store read between frames.
void applyMode(int modeInt);

// Apply validated per-cell-type reachability ranges (clamped to the MCM bounds; out-of-range
// ignored per R5.6). exteriorRange drives exterior + behaves-as-exterior cells; interiorRange
// drives true interiors. The wrapper selects between them each frame (see selectRangeForCell).
void applyExteriorRange(int range);
void applyInteriorRange(int range);

// Select which range is active for the current cell and rebuild the tier bands from it.
// hasWeather is MWBridge::CellHasWeather(): true for exterior + behaves-as-exterior cells,
// false for true interiors. Called once per frame by the wrapper before the cull pass.
void selectRangeForCell(bool hasWeather);

// Apply a validated Frame_Time_Budget in ms (clamped to [0.5, 20.0], R1.5/R5.2). This is the
// target the adaptive budget controller paces RT_AntiCull's added off-screen work against.
void applyFrameTimeBudgetMs(float ms);

// Read-only status for the MCM to surface (R7.6): is the hook actually installed, and what
// mode is effectively active. When init failed, effectiveMode reports the real fallback
// (Native_Frustum_Culling in this delivery vehicle — see note in rt_anticull.cpp), not the
// user's selection.
bool     hookInstalled();
CullMode effectiveMode();

// ---------------------------------------------------------------------------
// Pure decision logic — exposed for unit/parity testing against the Python oracle.
// These have no engine dependency and operate purely on values.
// ---------------------------------------------------------------------------

// Clamp a per-tier reachability range into [TierRangeMin, TierRangeMax] (R4.4).
int clampTierRange(int value);

// Squared Euclidean distance (the heuristic + reachability work in squared space).
float squaredDistance(const Vec3& a, const Vec3& b);

// Assign a camera-distance to exactly one tier (total function; FAR fallback). R3.1.
const SubmissionTier& assignTier(const Config& cfg, float distSquared);

// Conservative reachability predicate for a CONDITIONAL-tier node (R2/R4). Returns
// false when beyond tier.range, false when definitely terrain-occluded, else true.
bool reachable(const Config& cfg, const SubmissionTier& tier,
               const Vec3& cameraPos, const Vec3& boundCenter, float boundRadius);

} // namespace RTAntiCull

#endif // MGE_RTX
