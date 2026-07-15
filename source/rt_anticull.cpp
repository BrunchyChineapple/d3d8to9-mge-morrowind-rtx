/**
 * rt_anticull.cpp — native FrustumCull replacement implementing RT_AntiCull.
 *
 * Port of the validated rt_anticull/ decision logic. The pure functions
 * (assignTier / reachable / heuristic) mirror the Python module-for-module so the
 * property-test oracle (P1-P18) applies. The engine glue installs an absolute jmp over
 * FrustumCull @ 0x6EB480 and runs the per-node Kept_Set decision in its place.
 *
 * Calling convention: FrustumCull is __thiscall(NiNode* ecx, NiCullingContext* [esp+4])
 * and cleans 4 bytes of args (ret 4). The replacement is declared __fastcall so ecx is
 * delivered as the first parameter; the second __fastcall register (edx) is unused; the
 * original [esp+4] stack argument is the third parameter. One stack parameter makes the
 * compiler emit callee cleanup of 4 bytes, matching the engine's `ret 4` so the call
 * site's stack stays balanced.
 *
 * Hook mechanism: an absolute indirect jump `FF 25 <&g_jmpTarget>` (6 bytes) is written
 * over the FrustumCull prologue. Absolute (not rel32) so it is immune to the module
 * distance overflowing a signed 32-bit displacement — Morrowind is large-address-aware
 * (MCP/4GB), so a relative jmp to our DLL could exceed +/-2GB.
 */

#ifdef MGE_RTX

#ifndef D3D8TO9NOLOG
#define D3D8TO9NOLOG
#endif

#include "rt_anticull.h"
#include "support/log.h"

#include <windows.h>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

namespace RTAntiCull {

// ===========================================================================
// Config / tiers
// ===========================================================================

// Forward declaration: the constructor uses it to derive the default config's distance
// bands from its default range, so the default config is self-consistent.
static void applyRangeToTiers(Config& cfg, int range);

Config::Config() {
    // Three bands (NEAR / CONDITIONAL / FAR) with their kinds + resubmit intervals; the
    // distance boundaries are derived from the default range via applyRangeToTiers so the
    // default config matches exactly what the MCM would push for the same range.
    tierCount = 3;
    tiers[0] = SubmissionTier{ 0, TierKind::Near,        0.0f, 0.0f,
        Bounds::TierRangeDefault, Bounds::NearResubmitInterval };
    tiers[1] = SubmissionTier{ 1, TierKind::Conditional, 0.0f, 0.0f,
        Bounds::TierRangeDefault, Bounds::MinConditionalResubmitInterval };
    tiers[2] = SubmissionTier{ 2, TierKind::Far,         0.0f, FLT_MAX,
        Bounds::TierRangeMin, Bounds::MinConditionalResubmitInterval };
    applyRangeToTiers(*this, activeRange);
}

// Propagate the MCM RT_Reachability_Range into the tier bands. The MCM value is the "how
// much to keep around the player" dial; it sets the full off-screen keep radius. The bands
// scale WITH it so the slider can shrink the keep volume all the way to its floor (the old
// fixed 4096 NEAR shell + 4096 tier floor pinned the effective radius at ~one cell no matter
// how low the slider went). Layout for radius R (clamped to [TierRangeMin, TierRangeMax]):
//   NEAR        [0, near)            always keep; near = min(R/2, NearShellMaxWidth)
//   CONDITIONAL [near, R)            keep iff reachable
//   FAR         [R, +inf)            cull
// At the default R=8192 this reproduces the legacy bands exactly (NEAR [0,4096),
// CONDITIONAL [4096,8192)), so areas already tuned at higher ranges are unaffected.
static void applyRangeToTiers(Config& cfg, int range) {
    const int R = clampTierRange(range);
    int nearWidth = R / 2;
    if (nearWidth > Bounds::NearShellMaxWidth) nearWidth = Bounds::NearShellMaxWidth;

    for (int i = 0; i < cfg.tierCount; ++i) {
        switch (cfg.tiers[i].kind) {
            case TierKind::Near:
                cfg.tiers[i].nearDist = 0.0f;
                cfg.tiers[i].farDist  = static_cast<float>(nearWidth);
                cfg.tiers[i].range    = R;
                break;
            case TierKind::Conditional:
                cfg.tiers[i].nearDist = static_cast<float>(nearWidth);
                cfg.tiers[i].farDist  = static_cast<float>(R);
                cfg.tiers[i].range    = R;
                break;
            case TierKind::Far:
                cfg.tiers[i].nearDist = static_cast<float>(R);
                cfg.tiers[i].farDist  = FLT_MAX;
                break;
        }
    }
}

// ===========================================================================
// Pure decision logic (mirrors rt_anticull/*.py)
// ===========================================================================

int clampTierRange(int value) {
    if (value < Bounds::TierRangeMin) return Bounds::TierRangeMin;
    if (value > Bounds::TierRangeMax) return Bounds::TierRangeMax;
    return value;
}

float squaredDistance(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

// Coarse horizon/depth occlusion test (rt_anticull.reachability.default_occlusion_oracle).
// Position-only (never orientation) so membership is stable as the player looks around.
// Z is up in Morrowind world space.
static const float kHorizonDropMargin     = 4096.0f;
static const float kHorizonDistanceMargin = 8192.0f;

static bool definitelyOccluded(const Vec3& cameraPos, const Vec3& boundCenter, float boundRadius) {
    const float sphereTopZ      = boundCenter.z + boundRadius;
    const float dropBelowCamera = cameraPos.z - sphereTopZ;
    const float dx = cameraPos.x - boundCenter.x;
    const float dy = cameraPos.y - boundCenter.y;
    const float horizontal = std::sqrt(dx * dx + dy * dy);
    return dropBelowCamera > kHorizonDropMargin && horizontal > kHorizonDistanceMargin;
}

const SubmissionTier& assignTier(const Config& cfg, float distSquared) {
    // Bands are scanned nearest-first; compare in squared space to avoid a sqrt.
    int bestFarIdx = -1;
    for (int i = 0; i < cfg.tierCount; ++i) {
        const SubmissionTier& t = cfg.tiers[i];
        const float nearSq = t.nearDist * t.nearDist;
        const float farSq  = (t.farDist >= FLT_MAX) ? FLT_MAX : t.farDist * t.farDist;
        if (distSquared >= nearSq && distSquared < farSq) {
            return t;
        }
        if (t.kind == TierKind::Far && (bestFarIdx < 0 || t.nearDist > cfg.tiers[bestFarIdx].nearDist)) {
            bestFarIdx = i;
        }
    }
    // Total function: never leave a node unassigned. Prefer the config's furthest FAR
    // band; if none exists, a static catch-all FAR keeps the contract.
    if (bestFarIdx >= 0) return cfg.tiers[bestFarIdx];
    static const SubmissionTier kFarFallback{
        -1, TierKind::Far, 0.0f, FLT_MAX, Bounds::TierRangeMin,
        Bounds::MinConditionalResubmitInterval };
    return kFarFallback;
}

bool reachable(const Config& cfg, const SubmissionTier& tier,
               const Vec3& cameraPos, const Vec3& boundCenter, float boundRadius) {
    (void)cfg;
    const float rangeSq = static_cast<float>(tier.range) * static_cast<float>(tier.range);
    if (squaredDistance(cameraPos, boundCenter) > rangeSq) {
        return false; // beyond range (R2.5/R4.1) — checked first, short-circuits
    }
    if (definitelyOccluded(cameraPos, boundCenter, boundRadius)) {
        return false; // R4.2
    }
    return true; // conservative keep
}

// ===========================================================================
// Engine glue
// ===========================================================================

namespace {

// __thiscall thunk types for the engine methods we invoke.
typedef int      (__thiscall* AppCulledFn)(void* node);
typedef void     (__thiscall* DisplayFn)(void* node, void* camera);
typedef unsigned (__thiscall* PlaneTestFn)(void* niBound, void* plane);

constexpr int kPatchSize = 6; // FF 25 + abs32 (jmp dword ptr [&g_jmpTarget])

bool      g_initialized = false;
bool      g_hookInstalled = false; // true iff the FrustumCull patch is actually live
Config    g_config;
uint32_t  g_frameIndex  = 0;
int       g_keepSubtreeDepth = 0; // >0 while inside an off-screen subtree already kept this frame
uint8_t   g_savedBytes[kPatchSize] = {};
void*     g_hookTarget  = nullptr;
void*     g_jmpTarget   = nullptr; // holds &hookedFrustumCull; FF 25 jumps through this

// Config-sidecar polling. Re-reading a file every frame would be wasteful; poll on a
// throttled cadence so MCM edits land within ~1s. The path is resolved once relative to
// the running exe so it is the same directory the user drops d3d8.dll into.
constexpr uint32_t kConfigPollPeriodFrames = 60u;
char      g_configPath[MAX_PATH] = {};
FILETIME  g_configLastWrite = {};
bool      g_configPathResolved = false;

// Per-frame measurement counters (R8.1, R8.4). Counts are cheap increments on the cull
// path; they are always maintained. "kept" = nodes Displayed this frame; "culled" = nodes
// the hook dropped; "extra" = off-screen nodes kept by RT/heuristic logic BEYOND the native
// frustum set F (the RT-aware additions — direct evidence the Kept_Set is a superset of F).
// Current-frame counters accumulate during the cull pass; published counters hold the last
// completed frame so a reader never sees a half-counted frame.
uint32_t  g_curKept = 0, g_curCulled = 0, g_curExtra = 0;
uint32_t  g_pubKept = 0, g_pubCulled = 0, g_pubExtra = 0;
constexpr uint32_t kMetricsWritePeriodFrames = 30u;
constexpr const char* kMetricsFileName = "rt_anticull_metrics.txt";
char      g_metricsPath[MAX_PATH] = {};

// ---- Always-keep list (console-tagged base ids) --------------------------
// The MWSE-Lua control plane writes one base-object id per line to this sidecar. A node is
// force-kept (subtree, bypassing distance/reachability/cadence/mode) when its name CONTAINS
// any tagged id. We match on the node name (node+0x08) rather than a pointer because a
// reference's scene-node is named after its base object (live-observed item C, e.g.
// "light_com_sconce_02_128"); this needs no node-address channel from Lua. Matching is
// case-insensitive substring (the reference root may be "CLONE <id>" or "<id>"). Empty list
// (default) => the whole feature is a single bool check with zero per-node cost.
constexpr const char* kKeepFileName = "rt_anticull_keep.txt";
constexpr int kMaxKeepIds = 64;
constexpr int kMaxKeepIdLen = 64;
char      g_keepPath[MAX_PATH] = {};
char      g_keepIds[kMaxKeepIds][kMaxKeepIdLen] = {};
int       g_keepCount = 0;
FILETIME  g_keepLastWrite = {};

// ---- Adaptive budget controller (R1) -------------------------------------
// Holds RT_AntiCull's added off-screen-keep work within the Frame_Time_Budget by throttling
// how many CONDITIONAL off-screen nodes are admitted per frame. Faithful adaptation of
// rt_anticull/budget.py to the recursive hook: there is no upfront candidate list to sort, so
// the "growth cap" is a per-frame count of admitted CONDITIONAL nodes, and "added time" is the
// real wall-time spent in CONDITIONAL keep decisions (QPC-measured). NEAR, arm-camera, and the
// in-frustum floor F are never throttled — only the growable CONDITIONAL tier.
constexpr double  kSingleFrameCapMultiplier = 2.0;   // hard cap = 2 x budget (R1.4)
constexpr int     kRollingWindowSize        = 60;    // R1.1
constexpr int     kMinGrowthCap             = 0;     // can throttle all the way down
constexpr int     kMaxGrowthCap             = 100000;// effectively unbounded ceiling
constexpr int     kGrowthRelaxStep          = 64;    // candidates added per relax frame
constexpr double  kGrowthRelaxThreshold     = 0.9;   // relax only at/below 90% of budget

int       g_growthCap            = kMaxGrowthCap; // start relaxed; tightens on overrun
int       g_conditionalAdmitted  = 0;             // CONDITIONAL nodes admitted this frame
double    g_addedTimeThisFrameMs = 0.0;           // measured CONDITIONAL keep time this frame
double    g_rollingWindow[kRollingWindowSize] = {};
int       g_rollingCount         = 0;             // number of valid samples (<= window size)
int       g_rollingHead          = 0;             // next write index (circular)
double    g_pubAddedTimeMs       = 0.0;           // published last-frame added time (R8.2)
double    g_qpcToMs              = 0.0;           // QPC ticks -> ms; 0 until initialized

inline void* readVtable(void* node) { return *reinterpret_cast<void**>(node); }

inline void* vtableSlot(void* node, int byteOffset) {
    uint8_t* vt = reinterpret_cast<uint8_t*>(readVtable(node));
    return *reinterpret_cast<void**>(vt + byteOffset);
}

inline Vec3 readVec3(void* base, int byteOffset) {
    const float* p = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(base) + byteOffset);
    return Vec3{ p[0], p[1], p[2] };
}

inline float readFloat(void* base, int byteOffset) {
    return *reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(base) + byteOffset);
}

inline bool engineAppCulled(void* node) {
    AppCulledFn fn = reinterpret_cast<AppCulledFn>(vtableSlot(node, Engine::kVtbl_getAppCulled));
    return fn(node) != 0;
}

// Case-insensitive substring test: does haystack contain needle?
inline bool containsNoCase(const char* haystack, const char* needle) {
    for (const char* h = haystack; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b &&
               (tolower(static_cast<unsigned char>(*a)) == tolower(static_cast<unsigned char>(*b)))) {
            ++a; ++b;
        }
        if (*b == '\0') return true;
    }
    return false;
}

// Is this node on the always-keep list? Reads the node name (char* at node+0x08) and tests
// it against every tagged base id (case-insensitive substring — the reference root is named
// after its base object, e.g. "light_com_sconce_02_128" or "CLONE <id>"). Returns false fast
// when the list is empty (the common case), so the feature costs one int compare when unused.
inline bool nodeOnKeepList(void* node) {
    if (g_keepCount == 0) return false;
    const char* name = *reinterpret_cast<const char* const*>(
        reinterpret_cast<uint8_t*>(node) + Engine::kNode_name);
    if (!name || IsBadReadPtr(name, 1)) return false;
    for (int i = 0; i < g_keepCount; ++i) {
        if (containsNoCase(name, g_keepIds[i])) return true;
    }
    return false;
}

// The KEEP action: NI::AVObject::Display(node, camera) at vtable+0x78. This is the exact
// call the engine makes for every surviving node and the mechanism the shipping mod uses
// to force-keep. For a Node it recurses into children, which re-enters this hook per
// child; culling a node therefore correctly prunes its whole subtree.
inline void engineDisplay(void* node, void* camera) {
    DisplayFn fn = reinterpret_cast<DisplayFn>(vtableSlot(node, Engine::kVtbl_display));
    fn(node, camera);
}

// Display an off-screen node and everything beneath it as one coherent subtree. The depth
// guard makes every descendant take the "ancestor already committed" fast path in
// decideNode, so a node's children are never starved by their own cadence/reachability —
// the topmost off-screen node's decision governs the whole subtree for this frame.
inline void engineDisplayKeepSubtree(void* node, void* camera) {
    ++g_keepSubtreeDepth;
    engineDisplay(node, camera);
    --g_keepSubtreeDepth;
}

// Is the active camera the 1st-person arm camera? The v6 mod always-keeps through it so
// the 1st-person weapon/hands are never culled.
inline bool isArmCamera(void* ctx) {
    void* wc = *reinterpret_cast<void**>(Engine::kWorldController);
    if (!wc) return false;
    void* armCam = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(wc) + Engine::kWC_armCamera);
    return ctx == armCam;
}

// Replicate the engine's native frustum test: a node is in-frustum unless its bounding
// sphere is entirely OUTSIDE any one plane (PlaneTest == 2). This reproduces the native
// set F. The NiBound passed to PlaneTest is node+0x1C (center.xyz followed by radius).
// Returns true when the node should be kept by the frustum (intersects or is inside the
// view volume) — the same decision the original FrustumCull makes before recursing.
inline bool inFrustum(void* node, void* ctx) {
    const int planeCount = *reinterpret_cast<const int*>(
        reinterpret_cast<uint8_t*>(ctx) + Engine::kCtx_planeCount);
    void** planeArray = *reinterpret_cast<void***>(
        reinterpret_cast<uint8_t*>(ctx) + Engine::kCtx_planeArray);
    if (!planeArray || planeCount <= 0) {
        // Can't evaluate the frustum — keep conservatively (never cull on uncertainty).
        return true;
    }

    void* niBound = reinterpret_cast<uint8_t*>(node) + Engine::kNode_bound;
    PlaneTestFn planeTest = reinterpret_cast<PlaneTestFn>(Engine::kPlaneTest);
    for (int i = 0; i < planeCount; ++i) {
        void* plane = planeArray[i];
        if (!plane) continue;
        if (planeTest(niBound, plane) == 2u) {
            return false; // entirely outside this plane => outside the frustum
        }
    }
    return true;
}

// Stateless resubmission cadence (rt_anticull/resubmission.py, R3/R6).
//
// The effective interval is clamped to Remix's instance-retention window so a kept instance
// cannot lapse. NVIDIA's one-frame default therefore submits every kept off-screen subtree
// every frame. If a future build raises the retention window, the configured tier interval
// automatically restores staggered submissions without changing this path.
//
// This is STATELESS: instead of a per-node hashmap of last-submitted frames (which would put
// allocation + the item-C pointer-reuse/generation-guard problem on the hot recursive cull
// path), each node derives a deterministic phase from its own address and submits when
//   (frameIndex + phase) % interval == 0.
// The gap between consecutive submissions of a given node is therefore exactly `interval`,
// and the phase spreads different subtrees across different frames (R1.4 staggering) so
// per-frame cost is smooth. The interval is clamped to N so an instance can never lapse.
//
// SUBTREE COHERENCE: cadence is decided only at the TOPMOST off-screen node of a subtree.
// Because the engine recurses into children via Display, skipping a parent skips its whole
// subtree that frame; if each child independently re-rolled cadence, a child whose phase
// never aligned with an ancestor's "due" frame could be starved and lapse. The keep-subtree
// depth guard (see decideNode) ensures that once an off-screen node is displayed, every node
// beneath it is displayed in the same frame — so the topmost node's interval governs the
// whole subtree and no descendant is ever starved.
inline bool cadenceDueThisFrame(void* node, int resubmitInterval) {
    int interval = (resubmitInterval < Bounds::DefaultRetentionWindow)
                     ? resubmitInterval : Bounds::DefaultRetentionWindow;
    if (interval <= 1) {
        return true; // every frame
    }
    const uintptr_t p = reinterpret_cast<uintptr_t>(node);
    const uint32_t phase = static_cast<uint32_t>((p >> 4) % static_cast<uint32_t>(interval));
    return ((g_frameIndex + phase) % static_cast<uint32_t>(interval)) == 0u;
}

// ---- Adaptive budget controller helpers (rt_anticull/budget.py) ----------

inline double qpcNowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * g_qpcToMs;
}

// Admit (or defer) a CONDITIONAL off-screen node under the current growth cap. Returns true
// if the node should be kept this frame; false defers it (its instance stays alive via the
// cadence + Remix retention). NEAR / arm / in-frustum nodes never call this — only the
// growable CONDITIONAL tier is paced. The single-frame hard cap (2 x budget) is enforced via
// the measured added-time: once this frame's CONDITIONAL keep time hits the cap, further
// CONDITIONAL nodes are deferred regardless of the count cap (R1.4, refuse-don't-clamp).
inline bool budgetAdmitConditional() {
    const double cap = kSingleFrameCapMultiplier * static_cast<double>(g_config.frameTimeBudgetMs);
    if (g_addedTimeThisFrameMs >= cap) {
        return false; // hard cap reached this frame
    }
    if (g_conditionalAdmitted >= g_growthCap) {
        return false; // growth cap reached; defer surplus to a later frame
    }
    ++g_conditionalAdmitted;
    return true;
}

inline double budgetRollingAverageMs() {
    if (g_rollingCount == 0) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < g_rollingCount; ++i) sum += g_rollingWindow[i];
    return sum / static_cast<double>(g_rollingCount);
}

inline int budgetClampCap(int cap) {
    if (cap < kMinGrowthCap) return kMinGrowthCap;
    if (cap > kMaxGrowthCap) return kMaxGrowthCap;
    return cap;
}

// Bounded proportional controller (rt_anticull/budget.py::_update_growth_cap, R1.1/R1.3):
//   over budget         -> tighten toward floor(cap * budget / avg), at least one less
//   comfortably under    -> relax by kGrowthRelaxStep
//   within the deadband  -> hold (avoids oscillating across the budget boundary)
void budgetEndFrameUpdate() {
    // Record this frame's measured added time into the 60-sample ring.
    g_rollingWindow[g_rollingHead] = g_addedTimeThisFrameMs;
    g_rollingHead = (g_rollingHead + 1) % kRollingWindowSize;
    if (g_rollingCount < kRollingWindowSize) ++g_rollingCount;

    const double avg = budgetRollingAverageMs();
    const double budget = static_cast<double>(g_config.frameTimeBudgetMs);

    if (avg > budget) {
        int scaled = (avg > 0.0)
            ? static_cast<int>(static_cast<double>(g_growthCap) * budget / avg)
            : g_growthCap;
        int newCap = (scaled < g_growthCap - 1) ? scaled : (g_growthCap - 1);
        g_growthCap = budgetClampCap(newCap);
    } else if (avg <= kGrowthRelaxThreshold * budget) {
        g_growthCap = budgetClampCap(g_growthCap + kGrowthRelaxStep);
    }
    // else: deadband -> hold.
}

// Per-node Kept_Set decision. ctx is the NiCullingContext (== active NiCamera).
//
// Realizes K = F union {NEAR} union {CONDITIONAL and reachable} at the per-node grain:
//   - app-culled         -> never render (engine contract; checked even inside a kept subtree)
//   - inside kept subtree -> keep (an off-screen ancestor already committed; see below)
//   - arm camera         -> always keep (1st-person weapon must never cull)
//   - in frustum (set F) -> always keep, exactly like the native cull (THE FLOOR)
//   - off-screen NEAR    -> keep (whole subtree)
//   - off-screen CONDITIONAL -> keep (whole subtree) iff reachable AND cadence due this frame
//   - off-screen FAR / unreachable -> cull
//
// The frustum floor F is evaluated before the tier logic and recurses NORMALLY (children
// re-enter the hook and are each evaluated): an in-frustum parent may legitimately have an
// off-screen child, which then gets the RT keep-extra treatment. Keeping every in-frustum
// node guarantees RT_AntiCull only ever ADDS to what vanilla draws (R2.4).
//
// Off-screen kept nodes recurse via engineDisplayKeepSubtree so the whole subtree submits
// coherently this frame — the topmost off-screen node owns the cadence/reachability decision
// and no descendant is starved by its own phase.
//
// Metrics (R8.1/R8.4): counted at the top-level decision grain only (g_keepSubtreeDepth==0).
// Descendants displayed via the subtree guard are submission consequences, not policy
// decisions, so counting them would conflate subtree size with decision count. Counters
// measure nodes SUBMITTED this frame (Displayed) vs dropped; "extra" is the subset of
// submitted nodes that were off-screen (kept beyond the native frustum set F) — direct
// evidence of the superset behavior. app-culled nodes are an engine pre-filter present in
// every mode, so they are not counted (they carry no information about our policy).
void decideNode(void* node, void* ctx) {
    if (engineAppCulled(node)) {
        return;
    }

    // Inside an off-screen subtree an ancestor already chose to keep this frame: keep this
    // node too. Depth stays >0 through the engine's nested Display recursion, so the entire
    // subtree comes along coherently. Not counted — it is a consequence of the ancestor's
    // already-counted decision.
    if (g_keepSubtreeDepth > 0) {
        engineDisplay(node, ctx);
        return;
    }

    if (isArmCamera(ctx)) {
        ++g_curKept;
        engineDisplay(node, ctx);
        return;
    }

    // F: anything the native frustum keeps is kept unconditionally (the floor). Recurse
    // normally so off-screen children are independently re-evaluated for RT keep-extra.
    if (inFrustum(node, ctx)) {
        ++g_curKept;
        engineDisplay(node, ctx);
        return;
    }

    // Always-keep override: a console-tagged object is kept regardless of distance,
    // reachability, cadence, or mode (including Native) — the explicit "pin this outlier"
    // escape hatch. Subtree-kept so the whole tagged object comes along. Costs one int
    // compare when the keep-list is empty (the default).
    if (nodeOnKeepList(node)) {
        ++g_curKept; ++g_curExtra;
        engineDisplayKeepSubtree(node, ctx);
        return;
    }

    if (g_config.mode == CullMode::NativeFrustumCulling) {
        // K == F: off-screen nodes are culled exactly as vanilla would.
        ++g_curCulled;
        return;
    }

    const Vec3 cameraPos    = readVec3(ctx, Engine::kCtx_cameraPos);
    const Vec3 boundCenter  = readVec3(node, Engine::kNode_boundCenter);
    const float boundRadius = readFloat(node, Engine::kNode_boundRadius);

    if (g_config.mode == CullMode::AntiCullHeuristic) {
        // v6/v7 single-plane squared-distance test: keep iff (R + radius)^2 >= dist^2.
        const float r = static_cast<float>(g_config.activeRange) + boundRadius;
        if (squaredDistance(cameraPos, boundCenter) <= r * r) {
            ++g_curKept; ++g_curExtra;
            engineDisplayKeepSubtree(node, ctx);
        } else {
            ++g_curCulled;
        }
        return;
    }

    // RT_AntiCull (default): off-screen nodes kept by tier + reachability + cadence.
    const float distSq = squaredDistance(cameraPos, boundCenter);
    const SubmissionTier& tier = assignTier(g_config, distSq);

    if (tier.kind == TierKind::Near) {
        ++g_curKept; ++g_curExtra;
        engineDisplayKeepSubtree(node, ctx);
        return;
    }
    if (tier.kind == TierKind::Conditional) {
        // Pace the growable CONDITIONAL tier against the Frame_Time_Budget. The decision is
        // cheap; the real added cost is the Display traversal of the kept off-screen subtree
        // (work native culling would never do), so we measure THAT and fold it into the
        // frame's added-time accumulator. budgetAdmitConditional() defers surplus nodes once
        // the growth cap or the 2x-budget hard cap is hit — deferred nodes stay alive via
        // cadence + Remix retention (thinned, not dropped). The hard-cap check reads the time
        // accumulated by earlier keeps this frame, so it refuses before committing more work.
        if (reachable(g_config, tier, cameraPos, boundCenter, boundRadius)
            && cadenceDueThisFrame(node, tier.resubmitInterval)
            && budgetAdmitConditional()) {
            const double t0 = qpcNowMs();
            engineDisplayKeepSubtree(node, ctx);
            g_addedTimeThisFrameMs += qpcNowMs() - t0;
            ++g_curKept; ++g_curExtra;
        } else {
            ++g_curCulled;
        }
        return;
    }
    ++g_curCulled; // FAR: cull.
}

// FrustumCull replacement. __fastcall delivers ecx (node) as the first param; the unused
// edx slot is the second; the original [esp+4] stack argument (ctx) is the third. The
// stack parameter makes the compiler emit a `ret 4`-equivalent callee cleanup, matching
// the original calling convention so the engine's call-site stack stays balanced.
int __fastcall hookedFrustumCull(void* node, void* /*edx*/, void* ctx) {
    decideNode(node, ctx);
    return 0; // return value is ignored by all 7 callers (verified)
}

bool writeBytes(void* target, const void* bytes, size_t len) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    memcpy(target, bytes, len);
    DWORD ignore = 0;
    VirtualProtect(target, len, oldProtect, &ignore);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    return true;
}

// Resolve the config sidecar path next to the running executable (the directory the user
// drops d3d8.dll into). Done once; cached in g_configPath.
void resolveConfigPath() {
    if (g_configPathResolved) return;
    char exePath[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        g_configPathResolved = true; // give up; leave g_configPath empty
        return;
    }
    char* lastSep = strrchr(exePath, '\\');
    const size_t dirLen = lastSep ? static_cast<size_t>(lastSep - exePath + 1) : 0;
    char dir[MAX_PATH] = {};
    memcpy(dir, exePath, dirLen);
    _snprintf_s(g_configPath, sizeof(g_configPath), _TRUNCATE, "%s%s", dir, kConfigFileName);
    _snprintf_s(g_metricsPath, sizeof(g_metricsPath), _TRUNCATE, "%s%s", dir, kMetricsFileName);
    _snprintf_s(g_keepPath, sizeof(g_keepPath), _TRUNCATE, "%s%s", dir, kKeepFileName);
    g_configPathResolved = true;
}

// Internal validated appliers (the public apply* functions forward here). Kept in the
// anonymous namespace so parseConfigContents can call them.
void applyModeInternal(int modeInt) {
    switch (modeInt) {
        case 0: g_config.mode = CullMode::RtAntiCull;          break;
        case 1: g_config.mode = CullMode::AntiCullHeuristic;   break;
        case 2: g_config.mode = CullMode::NativeFrustumCulling; break;
        default: return; // reject unknown; retain previous (R5.6)
    }
}

inline bool rangeInBounds(int range) {
    return range >= Bounds::McmReachabilityRangeMin && range <= Bounds::McmReachabilityRangeMax;
}

void applyExteriorRangeInternal(int range) {
    if (!rangeInBounds(range)) return; // retain previous (R5.6)
    g_config.exteriorRange = range;
}

void applyInteriorRangeInternal(int range) {
    if (!rangeInBounds(range)) return; // retain previous (R5.6)
    g_config.interiorRange = range;
}

void applyBudgetInternal(float ms) {
    // Reject out-of-range; retain previous (R5.6, R1.5/R5.2 bounds).
    if (ms < Bounds::FrameTimeBudgetMinMs || ms > Bounds::FrameTimeBudgetMaxMs) {
        return;
    }
    g_config.frameTimeBudgetMs = ms;
}

// Parse the key=value sidecar and apply validated values. Unknown keys and malformed lines
// are ignored; out-of-range values are rejected by the appliers (R5.6). Tiny hand parser so
// the wrapper takes no JSON dependency — the MCM writes a trivial `key value` line format.
void parseConfigContents(const char* text) {
    const char* p = text;
    while (*p) {
        char key[32] = {};
        char val[32] = {};
        int consumed = 0;
        if (sscanf_s(p, " %31s %31s%n", key, static_cast<unsigned>(sizeof(key)),
                     val, static_cast<unsigned>(sizeof(val)), &consumed) == 2 && consumed > 0) {
            if (strcmp(key, "mode") == 0) {
                applyModeInternal(atoi(val));
            } else if (strcmp(key, "ext_range") == 0) {
                applyExteriorRangeInternal(atoi(val));
            } else if (strcmp(key, "int_range") == 0) {
                applyInteriorRangeInternal(atoi(val));
            } else if (strcmp(key, "budget") == 0) {
                applyBudgetInternal(static_cast<float>(atof(val)));
            }
            p += consumed;
        } else {
            // Skip to the next line on any unparseable content.
            while (*p && *p != '\n') ++p;
        }
        while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') ++p;
    }
}

// Poll the sidecar if it changed since last read. Throttled by the caller (beginFrame).
void pollConfigSidecar() {
    resolveConfigPath();
    if (g_configPath[0] == '\0') return;

    WIN32_FILE_ATTRIBUTE_DATA attr = {};
    if (!GetFileAttributesExA(g_configPath, GetFileExInfoStandard, &attr)) {
        return; // no sidecar yet — keep current (default) config
    }
    if (CompareFileTime(&attr.ftLastWriteTime, &g_configLastWrite) == 0) {
        return; // unchanged
    }

    HANDLE h = CreateFileA(g_configPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    char buf[512] = {};
    DWORD read = 0;
    if (ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
        buf[read] = '\0';
        parseConfigContents(buf);
        g_configLastWrite = attr.ftLastWriteTime;
    }
    CloseHandle(h);
}

// Poll the always-keep list sidecar if it changed. One base id per line; blank lines and
// surrounding whitespace ignored; capped at kMaxKeepIds. Rebuilt wholesale on each change
// (the MWSE-Lua side rewrites the whole file on edit / cell load).
void pollKeepList() {
    resolveConfigPath();
    if (g_keepPath[0] == '\0') return;

    WIN32_FILE_ATTRIBUTE_DATA attr = {};
    if (!GetFileAttributesExA(g_keepPath, GetFileExInfoStandard, &attr)) {
        g_keepCount = 0; // no file => empty list
        return;
    }
    if (CompareFileTime(&attr.ftLastWriteTime, &g_keepLastWrite) == 0) {
        return; // unchanged
    }

    HANDLE h = CreateFileA(g_keepPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    char buf[4096] = {};
    DWORD read = 0;
    int count = 0;
    if (ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
        buf[read] = '\0';
        const char* p = buf;
        while (*p && count < kMaxKeepIds) {
            char id[kMaxKeepIdLen] = {};
            int consumed = 0;
            if (sscanf_s(p, " %63[^\r\n]%n", id, static_cast<unsigned>(sizeof(id)), &consumed) == 1
                && consumed > 0) {
                // Trim trailing whitespace.
                int end = static_cast<int>(strlen(id));
                while (end > 0 && (id[end - 1] == ' ' || id[end - 1] == '\t')) id[--end] = '\0';
                if (end > 0) {
                    strncpy_s(g_keepIds[count], kMaxKeepIdLen, id, _TRUNCATE);
                    ++count;
                }
                p += consumed;
            } else {
                ++p;
            }
            while (*p == '\r' || *p == '\n') ++p;
        }
    }
    g_keepCount = count;
    g_keepLastWrite = attr.ftLastWriteTime;
    CloseHandle(h);
}

// Write the last completed frame's measurement counters to the metrics sidecar (R8.1/R8.2/
// R8.4), overwriting each time. A measurement harness (MWSE-Lua or external) reads this over a
// Test_Scene camera path to compare modes. Kept_Set count = kept; superset evidence = extra
// (off-screen nodes kept beyond F); added_ms = measured RT-added off-screen-keep time this
// frame (R8.2); growth_cap = the budget controller's current CONDITIONAL admission cap.
void writeMetricsSidecar() {
    resolveConfigPath();
    if (g_metricsPath[0] == '\0') return;

    HANDLE h = CreateFileA(g_metricsPath, GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    char buf[256] = {};
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "frame %u\nmode %d\nactive_range %d\nkept %u\nculled %u\nextra %u\nadded_ms %.3f\nbudget_ms %.2f\ngrowth_cap %d\n",
        g_frameIndex, modeToInt(g_config.mode), g_config.activeRange,
        g_pubKept, g_pubCulled, g_pubExtra,
        g_pubAddedTimeMs, g_config.frameTimeBudgetMs, g_growthCap);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(h, buf, static_cast<DWORD>(n), &written, nullptr);
    }
    CloseHandle(h);
}

} // namespace

bool initialize() {
    if (g_initialized) return true;

    // QPC frequency -> ms scale for the budget controller's added-time measurement.
    LARGE_INTEGER freq;
    if (QueryPerformanceFrequency(&freq) && freq.QuadPart != 0) {
        g_qpcToMs = 1000.0 / static_cast<double>(freq.QuadPart);
    }

    g_hookTarget = reinterpret_cast<void*>(Engine::kFrustumCull);

    // Sanity gate before committing to replacing engine code: the WorldController global
    // must be a readable pointer slot and the hook target must be executable. If basic
    // engine state isn't where we expect, bail and let the orchestrator fall back to the
    // heuristic (R7.4/R7.5) with the game still running.
    if (IsBadReadPtr(reinterpret_cast<void*>(Engine::kWorldController), sizeof(void*))) {
        LOG::logline("RT_AntiCull: WorldController global not readable; init aborted (heuristic fallback).");
        return false;
    }
    if (IsBadCodePtr(reinterpret_cast<FARPROC>(g_hookTarget))) {
        LOG::logline("RT_AntiCull: FrustumCull target not executable; init aborted (heuristic fallback).");
        return false;
    }

    memcpy(g_savedBytes, g_hookTarget, kPatchSize);

    // Absolute indirect jump: FF 25 <abs address of g_jmpTarget>; g_jmpTarget holds the
    // replacement's address. Distance-independent (safe under large-address-aware).
    g_jmpTarget = reinterpret_cast<void*>(&hookedFrustumCull);
    uint8_t patch[kPatchSize];
    patch[0] = 0xFF;
    patch[1] = 0x25;
    void* ptrSlot = &g_jmpTarget;
    memcpy(&patch[2], &ptrSlot, sizeof(uint32_t));

    if (!writeBytes(g_hookTarget, patch, kPatchSize)) {
        LOG::logline("RT_AntiCull: failed to write FrustumCull patch; init aborted (heuristic fallback).");
        return false;
    }

    g_initialized = true;
    g_hookInstalled = true;
    resolveConfigPath();
    pollConfigSidecar(); // apply any persisted MCM settings at startup
    pollKeepList();      // load any persisted always-keep tags at startup
    LOG::logline("RT_AntiCull: installed FrustumCull replacement @ 0x%p (mode=RT_AntiCull).", g_hookTarget);
    return true;
}

bool isInitialized() {
    return g_initialized;
}

void beginFrame() {
    if (!g_initialized) return;
    ++g_frameIndex;

    // Close the frame just rendered: run the budget controller on its measured added time
    // (adapts the growth cap for next frame), then publish counters + reset for the new frame
    // so readers see whole-frame totals.
    budgetEndFrameUpdate();
    g_pubAddedTimeMs = g_addedTimeThisFrameMs;
    g_pubKept   = g_curKept;
    g_pubCulled = g_curCulled;
    g_pubExtra  = g_curExtra;
    g_curKept = g_curCulled = g_curExtra = 0;
    g_addedTimeThisFrameMs = 0.0;
    g_conditionalAdmitted = 0;

    // Throttled config poll: pick up MCM edits within ~1s without per-frame file I/O.
    if ((g_frameIndex % kConfigPollPeriodFrames) == 0u) {
        pollConfigSidecar();
        pollKeepList();
    }
    // Throttled metrics dump for the measurement harness (R8). Off the hot path.
    if ((g_frameIndex % kMetricsWritePeriodFrames) == 0u) {
        writeMetricsSidecar();
    }
}

// ---------------------------------------------------------------------------
// Control plane (item E) — mode/range mapping, appliers, status.
// ---------------------------------------------------------------------------

int modeToInt(CullMode mode) {
    switch (mode) {
        case CullMode::RtAntiCull:           return 0;
        case CullMode::AntiCullHeuristic:    return 1;
        case CullMode::NativeFrustumCulling: return 2;
    }
    return 0;
}

CullMode modeFromInt(int value) {
    switch (value) {
        case 1:  return CullMode::AntiCullHeuristic;
        case 2:  return CullMode::NativeFrustumCulling;
        default: return CullMode::RtAntiCull;
    }
}

void applyMode(int modeInt) { applyModeInternal(modeInt); }
void applyExteriorRange(int range) { applyExteriorRangeInternal(range); }
void applyInteriorRange(int range) { applyInteriorRangeInternal(range); }
void applyFrameTimeBudgetMs(float ms) { applyBudgetInternal(ms); }

void selectRangeForCell(bool hasWeather) {
    // CellHasWeather() is true for exterior + behaves-as-exterior cells, false for true
    // interiors. Pick the matching range and rebuild the tier bands only when it changes, so
    // we don't churn the tiers every frame.
    const int desired = hasWeather ? g_config.exteriorRange : g_config.interiorRange;
    if (desired != g_config.activeRange) {
        g_config.activeRange = desired;
        applyRangeToTiers(g_config, desired);
    }
}

bool hookInstalled() {
    return g_hookInstalled;
}

CullMode effectiveMode() {
    // R7.6 adaptation for this delivery vehicle: if the hook never installed, the engine's
    // ORIGINAL FrustumCull runs — i.e. native frustum culling — because the heuristic only
    // exists inside our (absent) hook. Report that real state, not the user's selection.
    if (!g_hookInstalled) {
        return CullMode::NativeFrustumCulling;
    }
    return g_config.mode;
}

void shutdown() {
    if (g_initialized && g_hookTarget) {
        writeBytes(g_hookTarget, g_savedBytes, kPatchSize);
        g_initialized = false;
        g_hookInstalled = false;
        LOG::logline("RT_AntiCull: removed FrustumCull replacement; original bytes restored.");
    }
}

} // namespace RTAntiCull

#endif // MGE_RTX
