#ifdef MGE_RTX

#ifndef REMIX_ALLOW_X86
#define REMIX_ALLOW_X86
#endif

#include "retained_world.h"
#include "legacy_geometry_hash.h"

#include "ipc/client.h"
#include "ipc/retainedcatalog.h"
#include "mge/compositecache.h"
#include "dlcull_config.h"
#include "remix_api_test.h"
#include "remix_c.h"
#include "support/log.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kMaxCatalogCells = 65536;
constexpr std::uint32_t kMaxCatalogMeshes = 262144;
constexpr std::uint32_t kMaxCatalogPlacements = 1000000;
constexpr std::uint32_t kMaxCatalogBlobBytes = 512u * 1024u * 1024u;
constexpr std::uint64_t kBytesPerMiB = 1024ull * 1024ull;
constexpr std::uint32_t kDefaultCellRadius = 2;
constexpr std::uint32_t kMaxCellRadius = 10;
constexpr std::uint32_t kMaxLegacyDrawDistanceCells = 300;
constexpr std::uint32_t kDefaultMeshBudgetMB = 512;
constexpr std::uint32_t kDefaultTransitionBudgetMB = 1024;
constexpr std::uint32_t kDefaultCompositeBudgetMB = 0;
constexpr std::uint32_t kDefaultCommitsPerFrame = 2;
constexpr std::uint32_t kDefaultEvictionsPerFrame = 2;
constexpr std::uint32_t kMinMeshBudgetMB = 128;
constexpr std::uint32_t kMaxMeshBudgetMB = 2048;
constexpr std::uint32_t kMaxTransitionBudgetMB = 3072;
constexpr std::uint32_t kMinCompositeBudgetMB = 16;
constexpr std::uint32_t kMaxCompositeBudgetMB = 1024;
constexpr std::uint32_t kMaxCellChangesPerFrame = 8;
constexpr std::uint32_t kRequiredSettingsMask = (1u << 6) - 1;
constexpr std::uint32_t kCatalogRetryFrames = 60;
constexpr std::uint32_t kCatalogRefreshFrames = 300;
constexpr std::uint64_t kSettingsPollIntervalMs = 500;
constexpr float kCellSize = 8192.0f;
constexpr float kCellHysteresis = 512.0f;
constexpr std::uint32_t kAllowedMeshFlags =
    RetainedCatalog::MeshFlagHasAlpha |
    RetainedCatalog::MeshFlagAnimatedUv |
    RetainedCatalog::MeshFlagIndex32 |
    RetainedCatalog::MeshFlagCompositeDxt1;

struct RetainedWorldSettings {
    std::uint32_t radius = kDefaultCellRadius;
    std::uint32_t meshBudgetMB = kDefaultMeshBudgetMB;
    std::uint32_t transitionBudgetMB = kDefaultTransitionBudgetMB;
    std::uint32_t compositeBudgetMB = kDefaultCompositeBudgetMB;
    std::uint32_t commitsPerFrame = kDefaultCommitsPerFrame;
    std::uint32_t evictionsPerFrame = kDefaultEvictionsPerFrame;

    std::uint64_t meshBudgetBytes() const {
        return static_cast<std::uint64_t>(meshBudgetMB) * kBytesPerMiB;
    }

    std::uint64_t transitionBudgetBytes() const {
        return static_cast<std::uint64_t>(transitionBudgetMB) * kBytesPerMiB;
    }

    bool operator==(const RetainedWorldSettings& other) const {
        return radius == other.radius &&
            meshBudgetMB == other.meshBudgetMB &&
            transitionBudgetMB == other.transitionBudgetMB &&
            compositeBudgetMB == other.compositeBudgetMB &&
            commitsPerFrame == other.commitsPerFrame &&
            evictionsPerFrame == other.evictionsPerFrame;
    }
};

bool parseUnsignedSetting(const char* text, std::uint32_t& value) {
    if (!text || *text == '\0' || *text == '-') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

void logRetainedWorldSettings(
    const char* action,
    const RetainedWorldSettings& settings) {
    if (settings.compositeBudgetMB == 0) {
        LOG::logline(
            "RetainedWorld: %s radius=%u mesh=%u MiB transition=%u MiB composite=auto commits=%u evictions=%u",
            action,
            settings.radius,
            settings.meshBudgetMB,
            settings.transitionBudgetMB,
            settings.commitsPerFrame,
            settings.evictionsPerFrame);
    } else {
        LOG::logline(
            "RetainedWorld: %s radius=%u mesh=%u MiB transition=%u MiB composite=%u MiB commits=%u evictions=%u",
            action,
            settings.radius,
            settings.meshBudgetMB,
            settings.transitionBudgetMB,
            settings.compositeBudgetMB,
            settings.commitsPerFrame,
            settings.evictionsPerFrame);
    }
}

bool loadRetainedWorldSettings(
    RetainedWorldSettings& result,
    bool logInvalid) {
    RetainedWorldSettings settings;
    FILE* file = nullptr;
    if (fopen_s(&file, "mge_retained_world.cfg", "r") != 0 || !file) {
        return false;
    }

    std::uint32_t settingsMask = 0;
    char line[256] = {};
    while (std::fgets(line, sizeof(line), file)) {
        const std::size_t lineLength = std::strlen(line);
        if (lineLength == sizeof(line) - 1 && line[lineLength - 1] != '\n') {
            if (logInvalid) {
                LOG::logline("RetainedWorld: rejected overlong config line");
            }
            std::fclose(file);
            return false;
        }

        char key[64] = {};
        char text[64] = {};
        char extra[64] = {};
        const int fields = sscanf_s(
            line, "%63s %63s %63s",
            key, static_cast<unsigned>(sizeof(key)),
            text, static_cast<unsigned>(sizeof(text)),
            extra, static_cast<unsigned>(sizeof(extra)));
        if (fields <= 0 || key[0] == '#') {
            continue;
        }

        std::uint32_t* destination = nullptr;
        std::uint32_t settingBit = 0;
        if (_stricmp(key, "radius") == 0) {
            destination = &settings.radius;
            settingBit = 1u << 0;
        } else if (_stricmp(key, "mesh_budget_mb") == 0) {
            destination = &settings.meshBudgetMB;
            settingBit = 1u << 1;
        } else if (_stricmp(key, "transition_budget_mb") == 0) {
            destination = &settings.transitionBudgetMB;
            settingBit = 1u << 2;
        } else if (_stricmp(key, "composite_budget_mb") == 0) {
            destination = &settings.compositeBudgetMB;
            settingBit = 1u << 3;
        } else if (_stricmp(key, "commits_per_frame") == 0) {
            destination = &settings.commitsPerFrame;
            settingBit = 1u << 4;
        } else if (_stricmp(key, "evictions_per_frame") == 0) {
            destination = &settings.evictionsPerFrame;
            settingBit = 1u << 5;
        }
        if (!destination) {
            continue;
        }

        const bool hasValidSuffix = fields == 2 ||
            (fields == 3 && extra[0] == '#');
        std::uint32_t value = 0;
        if (!hasValidSuffix || !parseUnsignedSetting(text, value) ||
            (settingsMask & settingBit) != 0) {
            if (logInvalid) {
                LOG::logline(
                    "RetainedWorld: rejected malformed or duplicate setting '%s'",
                    key);
            }
            std::fclose(file);
            return false;
        }
        *destination = value;
        settingsMask |= settingBit;
    }
    const bool readFailed = std::ferror(file) != 0;
    std::fclose(file);
    if (readFailed || settingsMask != kRequiredSettingsMask) {
        return false;
    }

    settings.radius = std::clamp(settings.radius, 1u, kMaxCellRadius);
    settings.meshBudgetMB = std::clamp(
        settings.meshBudgetMB, kMinMeshBudgetMB, kMaxMeshBudgetMB);
    settings.transitionBudgetMB = std::clamp(
        settings.transitionBudgetMB,
        settings.meshBudgetMB,
        kMaxTransitionBudgetMB);
    if (settings.compositeBudgetMB != 0) {
        settings.compositeBudgetMB = std::clamp(
            settings.compositeBudgetMB,
            kMinCompositeBudgetMB,
            kMaxCompositeBudgetMB);
    }
    settings.commitsPerFrame = std::clamp(
        settings.commitsPerFrame, 1u, kMaxCellChangesPerFrame);
    settings.evictionsPerFrame = std::clamp(
        settings.evictionsPerFrame, 1u, kMaxCellChangesPerFrame);

    result = settings;
    return true;
}

struct CellKey {
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool operator==(const CellKey& other) const noexcept {
        return x == other.x && y == other.y;
    }
};

struct CellKeyHash {
    std::size_t operator()(const CellKey& key) const noexcept {
        const auto x = static_cast<std::uint32_t>(key.x);
        const auto y = static_cast<std::uint32_t>(key.y);
        return static_cast<std::size_t>((static_cast<std::uint64_t>(x) << 32) | y);
    }
};

enum class CellState {
    Absent,
    Loading,
    MeshReady,
    Committed,
    Evicting,
};

struct CatalogSnapshot {
    RetainedCatalog::Header header = {};
    std::vector<RetainedCatalog::Cell> cells;
    std::vector<RetainedCatalog::Mesh> meshes;
    std::vector<RetainedCatalog::Placement> placements;
    std::vector<std::uint8_t> blob;
    std::unordered_map<CellKey, std::size_t, CellKeyHash> cellByKey;
    std::unordered_map<std::uint64_t, std::size_t> meshByIdentity;

    void clear() {
        header = {};
        cells.clear();
        meshes.clear();
        placements.clear();
        blob.clear();
        cellByKey.clear();
        meshByIdentity.clear();
    }
};

struct MaterialResource {
    remixapi_MaterialHandle handle = nullptr;
    std::uint64_t legacyTextureHash = 0;
    ResidentCompositeCache* compositeCache = nullptr;
    CellId compositeCell = {};
    bool hasCompositeLease = false;
    std::uint32_t references = 0;
};

struct MeshResource {
    remixapi_MeshHandle handle = nullptr;
    std::uint64_t materialKey = 0;
    std::uint32_t references = 0;
    std::uint64_t bytes = 0;
};

struct InstanceResource {
    remixapi_InstanceHandle handle = nullptr;
    std::uint64_t identity = 0;
    std::uint64_t meshIdentity = 0;
};

struct CellRuntime {
    CellState state = CellState::Absent;
    std::vector<InstanceResource> instances;
    std::vector<std::uint64_t> meshes;
};

bool isRetainedCellActive(const CellRuntime& runtime) {
    return runtime.state != CellState::Absent ||
        !runtime.instances.empty() || !runtime.meshes.empty();
}

std::size_t countObsoleteCells(
    const std::unordered_map<CellKey, CellRuntime, CellKeyHash>& cells,
    const std::unordered_set<CellKey, CellKeyHash>& desired) {
    std::size_t count = 0;
    for (const auto& entry : cells) {
        if (isRetainedCellActive(entry.second) &&
            desired.find(entry.first) == desired.end()) {
            ++count;
        }
    }
    return count;
}

std::uint64_t hashBytes(
    const void* data,
    std::size_t bytes,
    std::uint64_t hash = 1469598103934665603ull) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t domainIdentity(std::uint64_t identity, std::uint64_t domain) {
    auto result = hashBytes(&domain, sizeof(domain));
    result = hashBytes(&identity, sizeof(identity), result);
    return result == 0 ? 1 : result;
}

bool checkedRange(std::uint32_t offset, std::uint32_t bytes, std::size_t limit) {
    return static_cast<std::uint64_t>(offset) + bytes <= limit;
}

bool checkedRecordRange(std::uint32_t first, std::uint32_t count, std::size_t limit) {
    return static_cast<std::uint64_t>(first) + count <= limit;
}

bool checkedProduct(std::uint32_t count, std::uint32_t stride, std::uint32_t expected) {
    return static_cast<std::uint64_t>(count) * stride == expected;
}

float halfToFloat(std::uint16_t value) {
    const std::uint32_t sign = (value >> 15) & 1;
    std::int32_t exponent = (value >> 10) & 0x1f;
    std::uint32_t mantissa = value & 0x3ff;
    std::uint32_t result = 0;

    if (exponent == 0) {
        if (mantissa == 0) {
            result = sign << 31;
        } else {
            exponent = -14;
            while ((mantissa & 0x400) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3ff;
            result = (sign << 31) |
                (static_cast<std::uint32_t>(exponent + 127) << 23) |
                (mantissa << 13);
        }
    } else if (exponent == 31) {
        result = (sign << 31) | 0x7f800000u | (mantissa << 13);
    } else {
        result = (sign << 31) |
            (static_cast<std::uint32_t>(exponent + 112) << 23) |
            (mantissa << 13);
    }

    float converted = 0.0f;
    std::memcpy(&converted, &result, sizeof(converted));
    return converted;
}

std::string decodePath(const std::uint8_t* bytes, std::uint32_t byteCount) {
    if (!bytes || byteCount == 0) {
        return {};
    }

    std::uint32_t encodedBytes = byteCount;
    const auto* terminator = static_cast<const std::uint8_t*>(
        std::memchr(bytes, '\0', byteCount));
    if (terminator) {
        if (terminator != bytes + byteCount - 1) {
            return {};
        }
        --encodedBytes;
    }
    if (encodedBytes == 0) {
        return {};
    }

    return std::string(
        reinterpret_cast<const char*>(bytes),
        static_cast<std::size_t>(encodedBytes));
}

class RetainedWorldManager {
public:
    bool initialize(IPC::Client& client, IDirect3DDevice9* device);
    void selectWorldspace(
        IPC::Client& client,
        const std::string& key,
        bool available,
        bool exterior);
    void setupCamera(
        const D3DXVECTOR4& eye,
        const D3DXMATRIX& view,
        const D3DXMATRIX& projection,
        float nearPlane,
        float farPlane,
        bool exterior);
    void prepareCompositeTransition(
        const D3DXVECTOR4& eye,
        const D3DXMATRIX& view,
        ResidentCompositeCache& composites);
    void reconcile(
        const D3DXVECTOR4& eye,
        const D3DXMATRIX& view,
        ResidentCompositeCache& composites);
    bool isCellCommitted(std::int32_t cellX, std::int32_t cellY) const;
    void requestCatalogRefresh();
    bool beforeDeviceReset();
    void afterDeviceReset(IDirect3DDevice9* device, bool resetSucceeded);
    bool shutdown();

private:
    bool allocateCatalogVectors(IPC::Client& client);
    bool releaseCatalogVectors();
    bool requestCatalog(CatalogSnapshot& result, bool& unchanged);
    bool validateCatalog(CatalogSnapshot& candidate) const;
    bool canCommitCell(CellKey key, ResidentCompositeCache& composites) const;
    bool commitCell(CellKey key, ResidentCompositeCache& composites);
    bool rollbackCell(CellKey key);
    bool evictCell(CellKey key);
    bool teardownCells();
    void clearCatalog();
    void pollSettings();
    float committedDrawDistanceCells(const D3DXVECTOR4& eye) const;
    const std::vector<CellKey>& chooseTargetCells(const D3DXVECTOR4& eye);
    CellKey chooseAnchor(const D3DXVECTOR4& eye);
    bool acquireMesh(
        const RetainedCatalog::Mesh& mesh,
        ResidentCompositeCache* composites,
        CellId terrainCell,
        std::uint64_t resourceLimit,
        std::uint64_t& acquiredIdentity);
    bool releaseMesh(std::uint64_t identity);
    bool createMaterial(
        const RetainedCatalog::Mesh& mesh,
        ResidentCompositeCache* composites,
        CellId terrainCell,
        std::uint64_t materialKey,
        MaterialResource& output);
    bool transcodeMesh(
        const RetainedCatalog::Mesh& mesh,
        std::vector<remixapi_HardcodedVertex>& vertices,
        std::vector<std::uint32_t>& indices) const;
    bool releaseMaterial(std::uint64_t materialKey);
    bool releaseUnusedMaterials();
    remixapi_Interface* remix() const;

    bool initialized_ = false;
    bool deviceReady_ = false;
    bool catalogActive_ = false;
    bool catalogAttempted_ = false;
    bool catalogRefreshRequested_ = false;
    bool anchorValid_ = false;
    bool targetValid_ = false;
    bool admissionDeferred_ = false;
    std::uint32_t catalogRetryCounter_ = 0;
    std::uint32_t refreshCounter_ = 0;
    std::uint64_t nextSettingsPollMs_ = 0;
    IPC::Client* client_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    std::string worldspaceKey_;
    CellKey anchor_ = {};
    CellKey targetAnchor_ = {};
    std::uint32_t targetRadius_ = 0;
    std::uint32_t targetInnerRadius_ = 0;
    std::uint64_t targetResourceBytes_ = 0;
    std::uint64_t targetCompositeBytes_ = 0;
    std::uint32_t targetCompositeBudgetMB_ = 64;
    float cameraDrawDistanceCells_ = 1.0f;
    RetainedWorldSettings settings_;
    std::vector<CellKey> targetCells_;

    IPC::VecId headerId_ = IPC::InvalidVector;
    IPC::VecId cellsId_ = IPC::InvalidVector;
    IPC::VecId meshesId_ = IPC::InvalidVector;
    IPC::VecId placementsId_ = IPC::InvalidVector;
    IPC::VecId blobId_ = IPC::InvalidVector;
    IPC::VecView<RetainedCatalog::Header> headerView_;
    IPC::VecView<RetainedCatalog::Cell> cellsView_;
    IPC::VecView<RetainedCatalog::Mesh> meshesView_;
    IPC::VecView<RetainedCatalog::Placement> placementsView_;
    IPC::VecView<std::uint8_t> blobView_;

    CatalogSnapshot catalog_;
    std::unordered_map<CellKey, CellRuntime, CellKeyHash> cells_;
    std::unordered_map<std::uint64_t, MeshResource> meshResources_;
    std::unordered_map<std::uint64_t, MaterialResource> materialResources_;
    std::unordered_set<std::uint64_t> liveInstanceIdentities_;
    std::uint64_t resourceBytes_ = 0;
};

RetainedWorldManager g_manager;

remixapi_Interface* RetainedWorldManager::remix() const {
    return RemixAPITest::getInterface();
}

bool RetainedWorldManager::initialize(IPC::Client& client, IDirect3DDevice9* device) {
    client_ = &client;
    device_ = device;
    deviceReady_ = device != nullptr;
    settings_ = RetainedWorldSettings{};
    if (loadRetainedWorldSettings(settings_, true)) {
        logRetainedWorldSettings("config", settings_);
    } else {
        LOG::logline(
            "RetainedWorld: using defaults (radius=%u, mesh=%u MiB, transition=%u MiB, composite=auto)",
            settings_.radius,
            settings_.meshBudgetMB,
            settings_.transitionBudgetMB);
    }
    nextSettingsPollMs_ = 0;
    targetValid_ = false;
    admissionDeferred_ = false;
    cameraDrawDistanceCells_ = 1.0f;

    if (!RemixAPITest::supportsRetainedInstances()) {
        initialized_ = false;
        LOG::logline("RetainedWorld: required Remix API slots unavailable; legacy rendering active");
        return false;
    }

    if (!headerView_.is_valid() && !allocateCatalogVectors(client)) {
        initialized_ = false;
        LOG::logline("RetainedWorld: catalog shared-vector allocation failed; legacy rendering active");
        return false;
    }

    initialized_ = true;
    return true;
}

void RetainedWorldManager::pollSettings() {
    if (!initialized_) {
        return;
    }

    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (now < nextSettingsPollMs_) {
        return;
    }
    nextSettingsPollMs_ = now + kSettingsPollIntervalMs;

    RetainedWorldSettings candidate;
    if (!loadRetainedWorldSettings(candidate, false) || candidate == settings_) {
        return;
    }

    settings_ = candidate;
    targetValid_ = false;
    logRetainedWorldSettings("applied live config", settings_);
}

bool RetainedWorldManager::allocateCatalogVectors(IPC::Client& client) {
    const bool hasRetainedIds =
        headerId_ != IPC::InvalidVector ||
        cellsId_ != IPC::InvalidVector ||
        meshesId_ != IPC::InvalidVector ||
        placementsId_ != IPC::InvalidVector ||
        blobId_ != IPC::InvalidVector;
    if (hasRetainedIds && !releaseCatalogVectors()) {
        LOG::logline("RetainedWorld: deferred catalog allocation until prior vectors are released");
        return false;
    }

    auto header = client.allocVecBlocking<RetainedCatalog::Header>(1, 1, 1);
    auto cells = client.allocVecBlocking<RetainedCatalog::Cell>(1024, kMaxCatalogCells, 1);
    auto meshes = client.allocVecBlocking<RetainedCatalog::Mesh>(1024, kMaxCatalogMeshes, 1);
    auto placements = client.allocVecBlocking<RetainedCatalog::Placement>(1024, kMaxCatalogPlacements, 1);
    auto blob = client.allocVecBlocking<std::uint8_t>(1024 * 1024, kMaxCatalogBlobBytes, 1);
    if (!header || !cells || !meshes || !placements || !blob) {
        auto releasePartial = [&client](auto& view, IPC::VecId& retainedId) {
            if (!view) {
                return;
            }
            retainedId = view->id();
            view.reset();
            if (client.freeVecBlocking(retainedId)) {
                retainedId = IPC::InvalidVector;
            } else {
                LOG::logline(
                    "RetainedWorld: retained partially allocated catalog vector %u for cleanup retry",
                    retainedId);
            }
        };
        releasePartial(blob, blobId_);
        releasePartial(placements, placementsId_);
        releasePartial(meshes, meshesId_);
        releasePartial(cells, cellsId_);
        releasePartial(header, headerId_);
        return false;
    }

    headerId_ = header->id();
    cellsId_ = cells->id();
    meshesId_ = meshes->id();
    placementsId_ = placements->id();
    blobId_ = blob->id();
    headerView_ = std::move(*header);
    cellsView_ = std::move(*cells);
    meshesView_ = std::move(*meshes);
    placementsView_ = std::move(*placements);
    blobView_ = std::move(*blob);
    return true;
}

bool RetainedWorldManager::releaseCatalogVectors() {
    headerView_ = {};
    cellsView_ = {};
    meshesView_ = {};
    placementsView_ = {};
    blobView_ = {};

    bool success = true;
    bool hostLost = false;
    auto freeId = [this, &success, &hostLost](IPC::VecId& id) {
        if (id == IPC::InvalidVector) {
            return;
        }
        if (hostLost) {
            id = IPC::InvalidVector;
            return;
        }
        if (!client_) {
            LOG::logline("RetainedWorld: no IPC client available to free catalog vector %u", id);
            success = false;
            return;
        }
        if (client_->freeVecBlocking(id)) {
            id = IPC::InvalidVector;
            return;
        }
        if (!client_->isServerActive()) {
            // A signaled process handle proves the host can no longer own or
            // access any of its vectors. Their local views were dropped above.
            LOG::logline("RetainedWorld: IPC host lost; abandoning catalog vector IDs");
            id = IPC::InvalidVector;
            hostLost = true;
            return;
        }
        LOG::logline("RetainedWorld: failed to free catalog vector %u", id);
        success = false;
    };
    freeId(headerId_);
    freeId(cellsId_);
    freeId(meshesId_);
    freeId(placementsId_);
    freeId(blobId_);
    return success;
}

template<typename T>
void copyView(IPC::VecView<T>& source, std::vector<T>& destination) {
    destination.clear();
    destination.reserve(source.size());
    for (std::uint32_t i = 0; i < source.size(); ++i) {
        destination.push_back(source[i]);
    }
}

bool RetainedWorldManager::requestCatalog(
    CatalogSnapshot& result,
    bool& unchanged) {
    result.clear();
    unchanged = false;
    const std::uint64_t knownGeneration =
        catalogActive_ ? catalog_.header.generation : 0;
    if (!client_ || !client_->getRetainedWorldCatalogBlocking(
            headerId_, cellsId_, meshesId_, placementsId_, blobId_,
            knownGeneration, unchanged)) {
        return false;
    }

    // The host generation changes on worldspace and dynamic-visibility mutations.
    // Equal generations leave the previously validated snapshot authoritative and, most
    // importantly, leave the large shared vectors untouched on the periodic probe path.
    if (unchanged) {
        return knownGeneration != 0;
    }
    if (headerView_.size() != 1) {
        return false;
    }

    result.header = headerView_[0];
    copyView(cellsView_, result.cells);
    copyView(meshesView_, result.meshes);
    copyView(placementsView_, result.placements);
    copyView(blobView_, result.blob);
    return validateCatalog(result);
}

bool RetainedWorldManager::validateCatalog(CatalogSnapshot& candidate) const {
    const auto& header = candidate.header;
    if (header.magic != RetainedCatalog::Magic ||
        header.version != RetainedCatalog::Version ||
        header.headerBytes != sizeof(RetainedCatalog::Header) ||
        header.flags != 0 || header.generation == 0 ||
        header.cellCount != candidate.cells.size() ||
        header.meshCount != candidate.meshes.size() ||
        header.placementCount != candidate.placements.size() ||
        header.blobBytes != candidate.blob.size() ||
        header.cellCount > kMaxCatalogCells ||
        header.meshCount > kMaxCatalogMeshes ||
        header.placementCount > kMaxCatalogPlacements ||
        header.blobBytes > kMaxCatalogBlobBytes) {
        return false;
    }

    std::uint64_t contentHash = hashBytes(
        candidate.cells.data(),
        candidate.cells.size() * sizeof(candidate.cells[0]));
    contentHash = hashBytes(
        candidate.meshes.data(),
        candidate.meshes.size() * sizeof(candidate.meshes[0]),
        contentHash);
    contentHash = hashBytes(
        candidate.placements.data(),
        candidate.placements.size() * sizeof(candidate.placements[0]),
        contentHash);
    contentHash = hashBytes(
        candidate.blob.data(), candidate.blob.size(), contentHash);
    if (contentHash != header.contentHash) {
        return false;
    }

    candidate.meshByIdentity.reserve(candidate.meshes.size());
    std::vector<std::uint8_t> terrainOwners(candidate.meshes.size(), 0);
    std::vector<std::uint8_t> placementOwners(candidate.placements.size(), 0);
    for (std::size_t i = 0; i < candidate.meshes.size(); ++i) {
        const auto& mesh = candidate.meshes[i];
        if (mesh.identity == 0 ||
            !candidate.meshByIdentity.emplace(mesh.identity, i).second ||
            (mesh.flags & ~kAllowedMeshFlags) != 0 ||
            !checkedRange(mesh.vertexOffset, mesh.vertexBytes, candidate.blob.size()) ||
            !checkedRange(mesh.indexOffset, mesh.indexBytes, candidate.blob.size()) ||
            mesh.vertexCount == 0 || mesh.indexCount == 0 ||
            mesh.vertexStride != 20 ||
            !checkedProduct(mesh.vertexCount, mesh.vertexStride, mesh.vertexBytes) ||
            !checkedProduct(mesh.indexCount, mesh.indexStride, mesh.indexBytes)) {
            return false;
        }

        if (mesh.category == RetainedCatalog::Category::Terrain) {
            if ((mesh.indexStride != 2 && mesh.indexStride != 4) ||
                ((mesh.flags & RetainedCatalog::MeshFlagIndex32) != 0) !=
                    (mesh.indexStride == 4) ||
                (mesh.flags & (RetainedCatalog::MeshFlagHasAlpha |
                               RetainedCatalog::MeshFlagAnimatedUv)) != 0 ||
                (mesh.flags & RetainedCatalog::MeshFlagCompositeDxt1) == 0 ||
                mesh.materialIdentity == 0 || mesh.materialBytes == 0 ||
                mesh.materialOffset != 0) {
                return false;
            }
        } else if (mesh.category == RetainedCatalog::Category::Static) {
            if (mesh.indexStride != 2 ||
                (mesh.flags & (RetainedCatalog::MeshFlagIndex32 |
                               RetainedCatalog::MeshFlagCompositeDxt1)) != 0 ||
                mesh.materialIdentity == 0 || mesh.materialBytes == 0 ||
                !checkedRange(mesh.materialOffset, mesh.materialBytes, candidate.blob.size())) {
                return false;
            }
        } else {
            return false;
        }
    }

    std::unordered_set<std::uint64_t> placementIdentities;
    placementIdentities.reserve(candidate.placements.size());
    for (const auto& placement : candidate.placements) {
        const auto prototype = candidate.meshByIdentity.find(placement.prototypeIdentity);
        if (placement.identity == 0 ||
            !placementIdentities.insert(placement.identity).second ||
            prototype == candidate.meshByIdentity.end() ||
            candidate.meshes[prototype->second].category != RetainedCatalog::Category::Static ||
            (placement.flags & ~1u) != 0 || placement.reserved != 0) {
            return false;
        }
        for (const float value : placement.transform) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
    }

    candidate.cellByKey.reserve(candidate.cells.size());
    for (std::size_t cellIndex = 0; cellIndex < candidate.cells.size(); ++cellIndex) {
        const auto& cell = candidate.cells[cellIndex];
        const CellKey key{ cell.cellX, cell.cellY };
        if (!candidate.cellByKey.emplace(key, cellIndex).second ||
            cell.terrainMeshCount == 0 ||
            !checkedRecordRange(
                cell.terrainMeshFirst,
                cell.terrainMeshCount,
                candidate.meshes.size()) ||
            !checkedRecordRange(
                cell.staticPlacementFirst,
                cell.staticPlacementCount,
                candidate.placements.size())) {
            return false;
        }

        for (std::uint32_t i = 0; i < cell.terrainMeshCount; ++i) {
            const auto meshIndex = cell.terrainMeshFirst + i;
            const auto& mesh = candidate.meshes[meshIndex];
            if (terrainOwners[meshIndex]++ != 0 ||
                mesh.category != RetainedCatalog::Category::Terrain ||
                mesh.cellX != cell.cellX || mesh.cellY != cell.cellY) {
                return false;
            }
        }
        for (std::uint32_t i = 0; i < cell.staticPlacementCount; ++i) {
            const auto placementIndex = cell.staticPlacementFirst + i;
            const auto& placement = candidate.placements[placementIndex];
            if (placementOwners[placementIndex]++ != 0 ||
                placement.cellX != cell.cellX || placement.cellY != cell.cellY) {
                return false;
            }
        }
    }

    for (std::size_t i = 0; i < candidate.meshes.size(); ++i) {
        const bool terrain = candidate.meshes[i].category == RetainedCatalog::Category::Terrain;
        if (terrain != (terrainOwners[i] == 1)) {
            return false;
        }
    }
    return std::all_of(
        placementOwners.begin(), placementOwners.end(),
        [](std::uint8_t owners) { return owners == 1; });
}

void RetainedWorldManager::requestCatalogRefresh() {
    catalogRefreshRequested_ = true;
    catalogRetryCounter_ = 0;
}

void RetainedWorldManager::selectWorldspace(
    IPC::Client& client,
    const std::string& key,
    bool available,
    bool exterior) {
    client_ = &client;
    if (!initialized_) {
        return;
    }

    if (!available || !exterior) {
        if ((catalogActive_ || !cells_.empty()) && !teardownCells()) {
            worldspaceKey_ = key;
            return;
        }
        clearCatalog();
        worldspaceKey_ = key;
        catalogAttempted_ = false;
        catalogRefreshRequested_ = false;
        catalogRetryCounter_ = 0;
        refreshCounter_ = 0;
        return;
    }

    const bool changedWorldspace = key != worldspaceKey_;
    if (changedWorldspace) {
        if ((catalogActive_ || !cells_.empty()) && !teardownCells()) {
            return;
        }
        clearCatalog();
        worldspaceKey_ = key;
        catalogAttempted_ = false;
        catalogRetryCounter_ = 0;
        refreshCounter_ = 0;
    }

    const bool explicitRefresh = catalogRefreshRequested_;
    if (!catalogActive_ && catalogAttempted_ && catalogRetryCounter_ > 0 &&
        !explicitRefresh) {
        --catalogRetryCounter_;
        if (catalogRetryCounter_ > 0) {
            return;
        }
    }

    const bool refreshDue = catalogActive_ && ++refreshCounter_ >= kCatalogRefreshFrames;
    if (catalogActive_ && !explicitRefresh && !refreshDue) {
        return;
    }

    CatalogSnapshot candidate;
    bool unchanged = false;
    catalogAttempted_ = true;
    refreshCounter_ = 0;
    if (!requestCatalog(candidate, unchanged)) {
        if (!catalogActive_) {
            catalogRetryCounter_ = kCatalogRetryFrames;
            LOG::logline("RetainedWorld: no valid exterior catalog; legacy rendering active");
        } else if (explicitRefresh) {
            // The host may already have applied a dynamic visibility change. End
            // suppression before retrying so an unavailable replacement catalog
            // cannot hide newly-enabled legacy geometry or retain stale instances.
            if (!teardownCells()) {
                LOG::logline("RetainedWorld: explicit refresh teardown incomplete; retrying fail-open");
            } else {
                // The old snapshot is no longer safe to recommit after the host
                // reported that a refresh was required but could not provide it.
                clearCatalog();
                LOG::logline("RetainedWorld: explicit refresh failed; legacy rendering active until retry");
            }
        } else {
            LOG::logline("RetainedWorld: periodic catalog refresh failed; preserving active catalog");
        }
        return;
    }

    if (unchanged) {
        catalogRefreshRequested_ = false;
        catalogRetryCounter_ = 0;
        return;
    }

    if (catalogActive_ &&
        candidate.header.generation == catalog_.header.generation &&
        candidate.header.contentHash == catalog_.header.contentHash) {
        catalogRefreshRequested_ = false;
        catalogRetryCounter_ = 0;
        return;
    }

    if (!teardownCells()) {
        catalogRefreshRequested_ = true;
        LOG::logline("RetainedWorld: catalog transition teardown incomplete; retrying refresh");
        return;
    }

    clearCatalog();
    catalog_ = std::move(candidate);
    for (const auto& pair : catalog_.cellByKey) {
        cells_.emplace(pair.first, CellRuntime{});
    }
    catalogActive_ = true;
    catalogRefreshRequested_ = false;
    catalogRetryCounter_ = 0;
    anchorValid_ = false;
    targetValid_ = false;
    LOG::logline(
        "RetainedWorld: catalog generation %llu validated (%u cells, %u meshes, %u placements)",
        static_cast<unsigned long long>(catalog_.header.generation),
        catalog_.header.cellCount,
        catalog_.header.meshCount,
        catalog_.header.placementCount);
}

void RetainedWorldManager::clearCatalog() {
    catalogActive_ = false;
    anchorValid_ = false;
    targetValid_ = false;
    admissionDeferred_ = false;
    targetRadius_ = 0;
    targetInnerRadius_ = 0;
    targetCells_.clear();
    targetResourceBytes_ = 0;
    targetCompositeBytes_ = 0;
    catalog_.clear();
    cells_.clear();
}

float RetainedWorldManager::committedDrawDistanceCells(
    const D3DXVECTOR4& eye) const {
    float drawDistanceCells = cameraDrawDistanceCells_;
    if (!initialized_ || !deviceReady_ || !catalogActive_) {
        return drawDistanceCells;
    }

    const CellKey cameraCell{
        static_cast<std::int32_t>(std::floor(eye.x / kCellSize)),
        static_cast<std::int32_t>(std::floor(eye.y / kCellSize))
    };
    for (const auto& entry : cells_) {
        if (entry.second.state != CellState::Committed) {
            continue;
        }
        const double dx = static_cast<double>(entry.first.x) - cameraCell.x;
        const double dy = static_cast<double>(entry.first.y) - cameraCell.y;
        const double radius = std::ceil(std::sqrt(dx * dx + dy * dy));
        drawDistanceCells = std::max(
            drawDistanceCells,
            static_cast<float>(std::min<double>(radius, kMaxCellRadius)));
    }
    return drawDistanceCells;
}

void RetainedWorldManager::setupCamera(
    const D3DXVECTOR4& eye,
    const D3DXMATRIX& view,
    const D3DXMATRIX& projection,
    float nearPlane,
    float farPlane,
    bool exterior) {
    pollSettings();

    float retainedFarPlane = farPlane;
    if (std::isfinite(farPlane) && farPlane > 0.0f) {
        cameraDrawDistanceCells_ = std::max(1.0f, farPlane / kCellSize);
        if (exterior) {
            retainedFarPlane = std::max(
                farPlane,
                committedDrawDistanceCells(eye) * kCellSize);
        }
    }

    auto* api = remix();
    if (!api || !api->SetupCamera || nearPlane <= 0.0f ||
        !std::isfinite(retainedFarPlane) || retainedFarPlane <= nearPlane ||
        projection._11 == 0.0f || projection._22 == 0.0f) {
        return;
    }

    remixapi_CameraInfoParameterizedEXT parameters = {};
    parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    parameters.position = { eye.x, eye.y, eye.z };
    parameters.right = { view._11, view._21, view._31 };
    parameters.up = { view._12, view._22, view._32 };
    parameters.forward = { view._13, view._23, view._33 };
    parameters.fovYInDegrees = std::atan(1.0f / projection._22) * 2.0f * 57.2957795131f;
    parameters.aspect = projection._22 / projection._11;
    parameters.nearPlane = nearPlane;
    parameters.farPlane = retainedFarPlane;

    remixapi_CameraInfo camera = {};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &parameters;
    camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
    api->SetupCamera(&camera);
}

CellKey RetainedWorldManager::chooseAnchor(const D3DXVECTOR4& eye) {
    const CellKey raw{
        static_cast<std::int32_t>(std::floor(eye.x / kCellSize)),
        static_cast<std::int32_t>(std::floor(eye.y / kCellSize))
    };
    if (!anchorValid_) {
        anchor_ = raw;
        anchorValid_ = true;
        targetValid_ = false;
        return anchor_;
    }
    if (raw == anchor_) {
        return anchor_;
    }

    const std::int64_t dx = static_cast<std::int64_t>(raw.x) - anchor_.x;
    const std::int64_t dy = static_cast<std::int64_t>(raw.y) - anchor_.y;
    if (std::abs(dx) + std::abs(dy) != 1) {
        anchor_ = raw;
        targetValid_ = false;
        return anchor_;
    }

    bool crossed = false;
    if (dx > 0) crossed = eye.x >= (anchor_.x + 1) * kCellSize + kCellHysteresis;
    if (dx < 0) crossed = eye.x < anchor_.x * kCellSize - kCellHysteresis;
    if (dy > 0) crossed = eye.y >= (anchor_.y + 1) * kCellSize + kCellHysteresis;
    if (dy < 0) crossed = eye.y < anchor_.y * kCellSize - kCellHysteresis;
    if (crossed) {
        anchor_ = raw;
        targetValid_ = false;
    }
    return anchor_;
}

const std::vector<CellKey>& RetainedWorldManager::chooseTargetCells(
    const D3DXVECTOR4& eye) {
    const CellKey anchor = chooseAnchor(eye);
    const auto innerRadius = static_cast<std::uint32_t>(std::clamp(
        std::ceil(cameraDrawDistanceCells_),
        1.0f,
        static_cast<float>(kMaxLegacyDrawDistanceCells)));
    const std::uint32_t radius = settings_.radius;
    if (targetValid_ && targetAnchor_ == anchor && targetRadius_ == radius &&
        targetInnerRadius_ == innerRadius) {
        return targetCells_;
    }

    targetValid_ = true;
    targetAnchor_ = anchor;
    targetRadius_ = radius;
    targetInnerRadius_ = innerRadius;
    targetCells_.clear();
    targetResourceBytes_ = 0;
    targetCompositeBytes_ = 0;

    struct Candidate {
        CellKey key;
        std::uint32_t distanceSq;
    };

    std::vector<Candidate> candidates;
    const int radiusInt = static_cast<int>(radius);
    candidates.reserve(static_cast<std::size_t>((radiusInt * 2 + 1) * (radiusInt * 2 + 1)));
    for (int dy = -radiusInt; dy <= radiusInt; ++dy) {
        for (int dx = -radiusInt; dx <= radiusInt; ++dx) {
            const auto distanceSq = static_cast<std::uint32_t>(dx * dx + dy * dy);
            if (distanceSq > radius * radius ||
                distanceSq <= innerRadius * innerRadius) {
                continue;
            }
            const std::int64_t x = static_cast<std::int64_t>(anchor.x) + dx;
            const std::int64_t y = static_cast<std::int64_t>(anchor.y) + dy;
            if (x < std::numeric_limits<std::int32_t>::min() ||
                x > std::numeric_limits<std::int32_t>::max() ||
                y < std::numeric_limits<std::int32_t>::min() ||
                y > std::numeric_limits<std::int32_t>::max()) {
                continue;
            }
            const CellKey key{ static_cast<std::int32_t>(x), static_cast<std::int32_t>(y) };
            if (catalog_.cellByKey.find(key) != catalog_.cellByKey.end()) {
                candidates.push_back({ key, distanceSq });
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.distanceSq != b.distanceSq) {
            return a.distanceSq < b.distanceSq;
        }
        if (a.key.y != b.key.y) {
            return a.key.y < b.key.y;
        }
        return a.key.x < b.key.x;
    });

    const std::uint64_t meshBudgetBytes = settings_.meshBudgetBytes();
    const std::uint64_t compositeBudgetBytes =
        static_cast<std::uint64_t>(
            settings_.compositeBudgetMB == 0
                ? kMaxCompositeBudgetMB
                : settings_.compositeBudgetMB) * kBytesPerMiB;
    std::unordered_set<std::uint64_t> requiredMeshes;
    std::uint32_t unsupportedCells = 0;
    bool budgetLimited = false;

    const auto checkedAdd = [](std::uint64_t& total, std::uint64_t value) {
        if (total > std::numeric_limits<std::uint64_t>::max() - value) {
            return false;
        }
        total += value;
        return true;
    };
    const auto meshBytes = [&](std::uint64_t identity, std::uint64_t& bytes) {
        const auto meshIndex = catalog_.meshByIdentity.find(identity);
        if (meshIndex == catalog_.meshByIdentity.end()) {
            return false;
        }
        const auto& mesh = catalog_.meshes[meshIndex->second];
        bytes = 0;
        return checkedAdd(
                   bytes,
                   static_cast<std::uint64_t>(mesh.vertexCount) *
                       sizeof(remixapi_HardcodedVertex)) &&
               checkedAdd(
                   bytes,
                   static_cast<std::uint64_t>(mesh.indexCount) *
                       sizeof(std::uint32_t));
    };

    for (const Candidate& candidate : candidates) {
        const auto catalogCell = catalog_.cellByKey.find(candidate.key);
        const auto& cell = catalog_.cells[catalogCell->second];
        std::vector<std::uint64_t> cellMeshes;
        cellMeshes.reserve(
            static_cast<std::size_t>(cell.terrainMeshCount) +
            cell.staticPlacementCount);
        std::uint64_t cellCompositeBytes = 0;
        bool retainable = true;

        for (std::uint32_t i = 0; i < cell.terrainMeshCount; ++i) {
            const auto& mesh = catalog_.meshes[cell.terrainMeshFirst + i];
            if ((mesh.flags & RetainedCatalog::MeshFlagCompositeDxt1) == 0 ||
                mesh.materialBytes == 0) {
                retainable = false;
                break;
            }
            cellMeshes.push_back(mesh.identity);
            cellCompositeBytes = std::max<std::uint64_t>(
                cellCompositeBytes, mesh.materialBytes);
        }
        if (!retainable) {
            ++unsupportedCells;
            continue;
        }

        for (std::uint32_t i = 0; i < cell.staticPlacementCount; ++i) {
            const auto& placement = catalog_.placements[cell.staticPlacementFirst + i];
            if ((placement.flags & 1u) == 0) {
                continue;
            }
            const auto meshIndex = catalog_.meshByIdentity.find(placement.prototypeIdentity);
            if (meshIndex == catalog_.meshByIdentity.end() ||
                (catalog_.meshes[meshIndex->second].flags &
                 RetainedCatalog::MeshFlagAnimatedUv) != 0) {
                retainable = false;
                break;
            }
            cellMeshes.push_back(placement.prototypeIdentity);
        }
        if (!retainable) {
            ++unsupportedCells;
            continue;
        }

        std::sort(cellMeshes.begin(), cellMeshes.end());
        cellMeshes.erase(std::unique(cellMeshes.begin(), cellMeshes.end()), cellMeshes.end());
        std::uint64_t addedResourceBytes = 0;
        for (const auto identity : cellMeshes) {
            if (requiredMeshes.find(identity) != requiredMeshes.end()) {
                continue;
            }
            std::uint64_t bytes = 0;
            if (!meshBytes(identity, bytes) || !checkedAdd(addedResourceBytes, bytes)) {
                retainable = false;
                break;
            }
        }
        if (!retainable) {
            ++unsupportedCells;
            continue;
        }

        if (addedResourceBytes > meshBudgetBytes ||
            targetResourceBytes_ > meshBudgetBytes - addedResourceBytes ||
            cellCompositeBytes > compositeBudgetBytes ||
            targetCompositeBytes_ > compositeBudgetBytes - cellCompositeBytes) {
            budgetLimited = true;
            break;
        }

        targetCells_.push_back(candidate.key);
        targetResourceBytes_ += addedResourceBytes;
        targetCompositeBytes_ += cellCompositeBytes;
        requiredMeshes.insert(cellMeshes.begin(), cellMeshes.end());
    }

    if (settings_.compositeBudgetMB != 0) {
        targetCompositeBudgetMB_ = settings_.compositeBudgetMB;
    } else {
        // Auto mode must cover the complete texture working set: the legacy renderer's
        // square visible-cell ring plus any retained cells outside that ring. Sizing only
        // from retained targets left the budget at 64 MiB whenever the retained annulus was
        // empty, forcing most otherwise-valid composites back to the province atlas.
        std::unordered_set<CellKey, CellKeyHash> compositeCells;
        compositeCells.reserve(catalog_.cellByKey.size());
        for (const auto& entry : catalog_.cellByKey) {
            const std::int64_t dx =
                static_cast<std::int64_t>(entry.first.x) - anchor.x;
            const std::int64_t dy =
                static_cast<std::int64_t>(entry.first.y) - anchor.y;
            if (std::abs(dx) <= innerRadius && std::abs(dy) <= innerRadius) {
                compositeCells.insert(entry.first);
            }
        }
        compositeCells.insert(targetCells_.begin(), targetCells_.end());

        std::uint64_t visibleCompositeBytes = 0;
        for (const CellKey key : compositeCells) {
            const auto catalogCell = catalog_.cellByKey.find(key);
            if (catalogCell == catalog_.cellByKey.end()) {
                continue;
            }
            const auto& cell = catalog_.cells[catalogCell->second];
            std::uint64_t cellCompositeBytes = 0;
            for (std::uint32_t i = 0; i < cell.terrainMeshCount; ++i) {
                cellCompositeBytes = std::max<std::uint64_t>(
                    cellCompositeBytes,
                    catalog_.meshes[cell.terrainMeshFirst + i].materialBytes);
            }
            if (cellCompositeBytes >
                std::numeric_limits<std::uint64_t>::max() - visibleCompositeBytes) {
                visibleCompositeBytes = std::numeric_limits<std::uint64_t>::max();
                break;
            }
            visibleCompositeBytes += cellCompositeBytes;
        }

        const std::uint64_t roundedMB =
            visibleCompositeBytes >
                    std::numeric_limits<std::uint64_t>::max() - (kBytesPerMiB - 1)
                ? std::numeric_limits<std::uint64_t>::max()
                : (visibleCompositeBytes + kBytesPerMiB - 1) / kBytesPerMiB;
        targetCompositeBudgetMB_ = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
            std::max<std::uint64_t>(64, roundedMB),
            kMinCompositeBudgetMB,
            kMaxCompositeBudgetMB));
    }

    LOG::logline(
        "RetainedWorld: target anchor=(%d,%d) inner=%u outer=%u selected %u/%u cells (%u unsupported%s), mesh %llu/%u MiB, composites %llu/%u MiB",
        anchor.x,
        anchor.y,
        innerRadius,
        radius,
        static_cast<unsigned>(targetCells_.size()),
        static_cast<unsigned>(candidates.size()),
        unsupportedCells,
        budgetLimited ? ", budget-limited" : "",
        static_cast<unsigned long long>((targetResourceBytes_ + kBytesPerMiB - 1) / kBytesPerMiB),
        settings_.meshBudgetMB,
        static_cast<unsigned long long>((targetCompositeBytes_ + kBytesPerMiB - 1) / kBytesPerMiB),
        targetCompositeBudgetMB_);
    return targetCells_;
}

void RetainedWorldManager::prepareCompositeTransition(
    const D3DXVECTOR4& eye,
    const D3DXMATRIX& view,
    ResidentCompositeCache& composites) {
    (void)view;
    VisibleCellSet transitionTargets;
    if (initialized_ && deviceReady_ && catalogActive_ && !catalogRefreshRequested_) {
        const auto& target = chooseTargetCells(eye);
        composites.setBudgetMB(targetCompositeBudgetMB_);
        std::unordered_set<CellKey, CellKeyHash> desired(target.begin(), target.end());

        std::vector<CellKey> obsolete;
        for (const auto& entry : cells_) {
            if (isRetainedCellActive(entry.second) &&
                desired.find(entry.first) == desired.end()) {
                obsolete.push_back(entry.first);
            }
        }
        std::sort(obsolete.begin(), obsolete.end(), [this](CellKey a, CellKey b) {
            const auto distanceSq = [this](CellKey key) {
                const std::int64_t dx = static_cast<std::int64_t>(key.x) - anchor_.x;
                const std::int64_t dy = static_cast<std::int64_t>(key.y) - anchor_.y;
                return dx * dx + dy * dy;
            };
            const auto aDistance = distanceSq(a);
            const auto bDistance = distanceSq(b);
            if (aDistance != bDistance) {
                return aDistance > bDistance;
            }
            if (a.y != b.y) {
                return a.y < b.y;
            }
            return a.x < b.x;
        });

        bool evictionSucceeded = true;
        std::uint32_t evicted = 0;
        for (const auto key : obsolete) {
            if (evicted >= settings_.evictionsPerFrame) {
                break;
            }
            if (!evictCell(key)) {
                evictionSucceeded = false;
                break;
            }
            ++evicted;
        }

        const std::size_t pendingObsolete = countObsoleteCells(cells_, desired);
        if (evictionSucceeded && pendingObsolete == 0) {
            if (admissionDeferred_) {
                LOG::logline(
                    "RetainedWorld: target admission ready (targets=%u, obsolete_pending=0)",
                    static_cast<unsigned>(target.size()));
            }
            admissionDeferred_ = false;
            transitionTargets.reserve(target.size());
            for (const auto key : target) {
                transitionTargets.insert(CellId{ key.x, key.y });
            }
        } else {
            if (!admissionDeferred_) {
                LOG::logline(
                    "RetainedWorld: target admission deferred (targets=%u, obsolete_pending=%u, evicted=%u, destroy_ok=%u)",
                    static_cast<unsigned>(target.size()),
                    static_cast<unsigned>(pendingObsolete),
                    evicted,
                    evictionSucceeded ? 1u : 0u);
            }
            admissionDeferred_ = true;
        }
    }
    composites.setTransitionTargets(transitionTargets);
    composites.trimToBudget();
}

bool RetainedWorldManager::transcodeMesh(
    const RetainedCatalog::Mesh& mesh,
    std::vector<remixapi_HardcodedVertex>& vertices,
    std::vector<std::uint32_t>& indices) const {
    vertices.assign(mesh.vertexCount, remixapi_HardcodedVertex{});
    indices.resize(mesh.indexCount);
    const auto* vertexBytes = catalog_.blob.data() + mesh.vertexOffset;
    const auto* indexBytes = catalog_.blob.data() + mesh.indexOffset;

    if (mesh.category == RetainedCatalog::Category::Static) {
        for (std::uint32_t i = 0; i < mesh.vertexCount; ++i) {
            const auto* source = vertexBytes + static_cast<std::size_t>(i) * mesh.vertexStride;
            std::uint16_t position[3] = {};
            std::uint16_t uv[2] = {};
            std::uint32_t color = 0;
            std::memcpy(position, source, sizeof(position));
            std::memcpy(&color, source + 12, sizeof(color));
            std::memcpy(uv, source + 16, sizeof(uv));

            auto& vertex = vertices[i];
            vertex.position[0] = halfToFloat(position[0]);
            vertex.position[1] = halfToFloat(position[1]);
            vertex.position[2] = halfToFloat(position[2]);
            vertex.normal[0] = (source[8] / 255.0f) * 2.0f - 1.0f;
            vertex.normal[1] = (source[9] / 255.0f) * 2.0f - 1.0f;
            vertex.normal[2] = (source[10] / 255.0f) * 2.0f - 1.0f;
            vertex.texcoord[0] = halfToFloat(uv[0]);
            vertex.texcoord[1] = halfToFloat(uv[1]);
            vertex.color = color;
        }
    } else {
        const float cellMinX = mesh.cellX * kCellSize;
        const float cellMinY = mesh.cellY * kCellSize;
        for (std::uint32_t i = 0; i < mesh.vertexCount; ++i) {
            const auto* source = vertexBytes + static_cast<std::size_t>(i) * mesh.vertexStride;
            auto& vertex = vertices[i];
            std::memcpy(vertex.position, source, sizeof(vertex.position));
            vertex.normal[0] = (source[16] / 255.0f) * 2.0f - 1.0f;
            vertex.normal[1] = (source[17] / 255.0f) * 2.0f - 1.0f;
            vertex.normal[2] = (source[18] / 255.0f) * 2.0f - 1.0f;
            // Preserve the closed [0,1] cell domain. Exact far-edge vertices are common in the
            // generated mesh; modulo wrapping maps their 1.0 coordinate to 0.0 and makes each
            // boundary triangle interpolate backward across almost the entire composite.
            const float u = std::clamp(
                (vertex.position[0] - cellMinX) / kCellSize, 0.0f, 1.0f);
            const float v = std::clamp(
                (vertex.position[1] - cellMinY) / kCellSize, 0.0f, 1.0f);
            vertex.texcoord[0] = u;
            vertex.texcoord[1] = 1.0f - v;
            vertex.color = 0xffffffffu;
        }
    }

    for (std::uint32_t i = 0; i < mesh.indexCount; ++i) {
        std::uint32_t index = 0;
        if (mesh.indexStride == 4) {
            std::memcpy(&index, indexBytes + static_cast<std::size_t>(i) * 4, 4);
        } else {
            std::uint16_t index16 = 0;
            std::memcpy(&index16, indexBytes + static_cast<std::size_t>(i) * 2, 2);
            index = index16;
        }
        if (index >= mesh.vertexCount) {
            return false;
        }
        indices[i] = index;
    }
    return true;
}

bool RetainedWorldManager::createMaterial(
    const RetainedCatalog::Mesh& mesh,
    ResidentCompositeCache* composites,
    CellId terrainCell,
    std::uint64_t materialKey,
    MaterialResource& output) {
    auto* api = remix();
    if (!api || !api->CreateMaterial) {
        return false;
    }

    bool hasCompositeLease = false;
    std::uint64_t textureHash = 0;
    std::array<wchar_t, 24> textureHashPath = {};
    if (mesh.category == RetainedCatalog::Category::Terrain) {
        if (!composites) {
            return false;
        }
        auto* terrainTexture = composites->pin(terrainCell);
        if (!terrainTexture) {
            return false;
        }
        hasCompositeLease = true;

        if (!api->dxvk_GetTextureHash ||
            api->dxvk_GetTextureHash(terrainTexture, &textureHash) != REMIXAPI_ERROR_CODE_SUCCESS ||
            textureHash == 0) {
            composites->unpin(terrainCell);
            return false;
        }
    } else {
        const auto albedoPath = decodePath(
            catalog_.blob.data() + mesh.materialOffset,
            mesh.materialBytes);
        if (albedoPath.empty()) {
            return false;
        }

        auto* staticTexture = static_cast<IDirect3DTexture9*>(
            const_cast<void*>(dlFindDistantTexture(albedoPath.c_str())));
        if (!staticTexture) {
            LOG::logline(
                "RetainedWorld: static texture name not registered for '%s'; keeping legacy rendering",
                albedoPath.c_str());
            return false;
        }
        if (!api->dxvk_GetTextureHash) {
            LOG::logline(
                "RetainedWorld: texture hash API unavailable for '%s'; keeping legacy rendering",
                albedoPath.c_str());
            return false;
        }
        const auto hashResult = api->dxvk_GetTextureHash(
            staticTexture, &textureHash);
        if (hashResult != REMIXAPI_ERROR_CODE_SUCCESS || textureHash == 0) {
            LOG::logline(
                "RetainedWorld: texture hash unavailable for '%s' (error %u); keeping legacy rendering",
                albedoPath.c_str(),
                static_cast<unsigned>(hashResult));
            return false;
        }
    }
    swprintf_s(
        textureHashPath.data(), textureHashPath.size(),
        L"0x%016llX", static_cast<unsigned long long>(textureHash));

    const bool hasAlpha = (mesh.flags & RetainedCatalog::MeshFlagHasAlpha) != 0;
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = { 1.0f, 1.0f, 1.0f };
    opaque.opacityConstant = 1.0f;
    opaque.roughnessConstant = 1.0f;
    opaque.metallicConstant = 0.0f;
    opaque.alphaTestType = hasAlpha ? 6 : 7;
    opaque.alphaReferenceValue = 128;

    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    // The API hash is the external material handle. Keep it aligned with the
    // wrapper's reference-counted material key so distinct alpha/category/cell
    // materials never destroy one another. Texture replacement matching remains
    // keyed from the resolved albedo image hash in the Remix submit path.
    info.hash = materialKey;
    info.albedoTexture = textureHashPath.data();
    info.filterMode = 1;
    // Remix sampler values are Clamp=0 and Repeat=1. Terrain composites use the closed
    // per-cell UV domain above; statics keep their original tiled-repeat behavior.
    const std::uint8_t wrapMode =
        mesh.category == RetainedCatalog::Category::Terrain ? 0u : 1u;
    info.wrapModeU = wrapMode;
    info.wrapModeV = wrapMode;

    if (api->CreateMaterial(&info, &output.handle) != REMIXAPI_ERROR_CODE_SUCCESS ||
        !output.handle) {
        if (hasCompositeLease) {
            composites->unpin(terrainCell);
        }
        output = {};
        return false;
    }
    output.legacyTextureHash =
        mesh.category == RetainedCatalog::Category::Static ? textureHash : 0;
    output.compositeCache = hasCompositeLease ? composites : nullptr;
    output.compositeCell = terrainCell;
    output.hasCompositeLease = hasCompositeLease;
    output.references = 1;
    return true;
}

bool RetainedWorldManager::acquireMesh(
    const RetainedCatalog::Mesh& mesh,
    ResidentCompositeCache* composites,
    CellId terrainCell,
    std::uint64_t resourceLimit,
    std::uint64_t& acquiredIdentity) {
    acquiredIdentity = mesh.identity;
    auto existing = meshResources_.find(mesh.identity);
    if (existing != meshResources_.end()) {
        ++existing->second.references;
        return true;
    }
    if ((mesh.flags & RetainedCatalog::MeshFlagAnimatedUv) != 0) {
        return false;
    }

    const std::uint64_t materialDomain =
        mesh.category == RetainedCatalog::Category::Terrain ? 0x5445525241494eull :
        ((mesh.flags & RetainedCatalog::MeshFlagHasAlpha) ? 0x53544154494341ull : 0x5354415449434full);
    std::uint64_t materialKey = domainIdentity(mesh.materialIdentity, materialDomain);
    if (mesh.category == RetainedCatalog::Category::Terrain) {
        const auto cellIdentity =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(terrainCell.x)) << 32) |
            static_cast<std::uint32_t>(terrainCell.y);
        materialKey = domainIdentity(materialKey, cellIdentity);
    }

    auto material = materialResources_.find(materialKey);
    if (material != materialResources_.end()) {
        ++material->second.references;
    } else {
        MaterialResource created;
        if (!createMaterial(mesh, composites, terrainCell, materialKey, created)) {
            return false;
        }
        material = materialResources_.emplace(materialKey, created).first;
    }

    std::vector<remixapi_HardcodedVertex> vertices;
    std::vector<std::uint32_t> indices;
    if (!transcodeMesh(mesh, vertices, indices)) {
        releaseMaterial(materialKey);
        return false;
    }
    std::uint64_t replacementHash = 0;
    if (mesh.category == RetainedCatalog::Category::Static) {
        LegacyGeometryHash::TriangleList legacyGeometry;
        const auto* sourceIndices = catalog_.blob.data() + mesh.indexOffset;
        if (material->second.legacyTextureHash == 0 ||
            !LegacyGeometryHash::computeTriangleList(
                vertices.data(), vertices.size() * sizeof(vertices[0]),
                static_cast<std::uint32_t>(sizeof(vertices[0])), 0,
                sourceIndices, mesh.indexBytes, mesh.indexStride, 0, mesh.indexCount,
                legacyGeometry)) {
            releaseMaterial(materialKey);
            return false;
        }
        replacementHash = legacyGeometry.geometryHash ^ material->second.legacyTextureHash;
        if (replacementHash == 0) {
            releaseMaterial(materialKey);
            return false;
        }
    }

    const std::uint64_t resourceBytes =
        vertices.size() * sizeof(vertices[0]) + indices.size() * sizeof(indices[0]);
    if (resourceBytes > resourceLimit || resourceBytes_ > resourceLimit - resourceBytes) {
        releaseMaterial(materialKey);
        return false;
    }

    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = vertices.data();
    surface.vertices_count = vertices.size();
    surface.indices_values = indices.data();
    surface.indices_count = indices.size();
    surface.material = material->second.handle;

    remixapi_MeshInfoReplacementEXT replacement = {};
    if (replacementHash != 0) {
        replacement.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO_REPLACEMENT_EXT;
        replacement.replacementHash = replacementHash;
    }

    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.pNext = replacementHash != 0 ? &replacement : nullptr;
    info.hash = mesh.identity;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;

    auto* api = remix();
    MeshResource created;
    if (!api || !api->CreateMesh ||
        api->CreateMesh(&info, &created.handle) != REMIXAPI_ERROR_CODE_SUCCESS ||
        !created.handle) {
        releaseMaterial(materialKey);
        return false;
    }
    created.materialKey = materialKey;
    created.references = 1;
    created.bytes = resourceBytes;
    meshResources_.emplace(mesh.identity, created);
    resourceBytes_ += resourceBytes;
    return true;
}

bool RetainedWorldManager::releaseMaterial(std::uint64_t materialKey) {
    auto resource = materialResources_.find(materialKey);
    if (resource == materialResources_.end()) {
        return true;
    }
    if (resource->second.references > 0) {
        --resource->second.references;
    }
    if (resource->second.references != 0) {
        return true;
    }

    auto* api = remix();
    if (!api || !api->DestroyMaterial || !resource->second.handle) {
        return false;
    }
    const auto result = api->DestroyMaterial(resource->second.handle);
    if (result != REMIXAPI_ERROR_CODE_SUCCESS) {
        LOG::logline(
            "RetainedWorld: DestroyMaterial failed for key %llu (error %u)",
            static_cast<unsigned long long>(materialKey),
            static_cast<unsigned>(result));
        return false;
    }
    if (resource->second.hasCompositeLease && resource->second.compositeCache) {
        resource->second.compositeCache->unpin(resource->second.compositeCell);
    }
    materialResources_.erase(resource);
    return true;
}

bool RetainedWorldManager::releaseUnusedMaterials() {
    std::vector<std::uint64_t> unused;
    for (const auto& resource : materialResources_) {
        if (resource.second.references == 0) {
            unused.push_back(resource.first);
        }
    }

    bool success = true;
    for (const auto materialKey : unused) {
        success = releaseMaterial(materialKey) && success;
    }
    return success;
}

bool RetainedWorldManager::releaseMesh(std::uint64_t identity) {
    auto resource = meshResources_.find(identity);
    if (resource == meshResources_.end()) {
        return true;
    }
    if (resource->second.references > 1) {
        --resource->second.references;
        return true;
    }

    auto* api = remix();
    if (!api || !api->DestroyMesh || !resource->second.handle) {
        return false;
    }
    const auto result = api->DestroyMesh(resource->second.handle);
    if (result != REMIXAPI_ERROR_CODE_SUCCESS) {
        LOG::logline(
            "RetainedWorld: DestroyMesh failed for identity %llu (error %u)",
            static_cast<unsigned long long>(identity),
            static_cast<unsigned>(result));
        return false;
    }

    resourceBytes_ -= resource->second.bytes;
    const auto materialKey = resource->second.materialKey;
    meshResources_.erase(resource);
    releaseMaterial(materialKey);
    return true;
}

bool RetainedWorldManager::canCommitCell(
    CellKey key,
    ResidentCompositeCache& composites) const {
    const auto catalogCell = catalog_.cellByKey.find(key);
    if (catalogCell == catalog_.cellByKey.end()) {
        return false;
    }
    const auto& cell = catalog_.cells[catalogCell->second];
    auto* api = remix();
    if (!api) {
        return false;
    }

    auto* texture = composites.lookup(CellId{ key.x, key.y });
    for (std::uint32_t i = 0; i < cell.terrainMeshCount; ++i) {
        const auto& mesh = catalog_.meshes[cell.terrainMeshFirst + i];
        if ((mesh.flags & RetainedCatalog::MeshFlagCompositeDxt1) == 0 ||
            mesh.materialBytes == 0) {
            return false;
        }
        std::uint64_t textureHash = 0;
        if (!texture || !api->dxvk_GetTextureHash ||
            api->dxvk_GetTextureHash(texture, &textureHash) != REMIXAPI_ERROR_CODE_SUCCESS ||
            textureHash == 0) {
            return false;
        }
    }

    for (std::uint32_t i = 0; i < cell.staticPlacementCount; ++i) {
        const auto& placement = catalog_.placements[cell.staticPlacementFirst + i];
        if ((placement.flags & 1u) == 0) {
            continue;
        }
        const auto mesh = catalog_.meshByIdentity.find(placement.prototypeIdentity);
        if (mesh == catalog_.meshByIdentity.end() ||
            (catalog_.meshes[mesh->second].flags & RetainedCatalog::MeshFlagAnimatedUv) != 0) {
            return false;
        }
    }
    return true;
}

bool RetainedWorldManager::commitCell(
    CellKey key,
    ResidentCompositeCache& composites) {
    LARGE_INTEGER performanceFrequency = {};
    LARGE_INTEGER commitStart = {};
    QueryPerformanceFrequency(&performanceFrequency);
    QueryPerformanceCounter(&commitStart);
    const auto elapsedMicroseconds =
        [&performanceFrequency](const LARGE_INTEGER& start, const LARGE_INTEGER& end)
            -> std::uint64_t {
            if (performanceFrequency.QuadPart <= 0 || end.QuadPart <= start.QuadPart) {
                return 0;
            }
            return static_cast<std::uint64_t>(
                (end.QuadPart - start.QuadPart) * 1000000ll /
                performanceFrequency.QuadPart);
        };

    auto runtimeIt = cells_.find(key);
    const auto catalogCell = catalog_.cellByKey.find(key);
    if (runtimeIt == cells_.end() || catalogCell == catalog_.cellByKey.end()) {
        return false;
    }
    auto& runtime = runtimeIt->second;
    if (runtime.state == CellState::Committed) {
        return true;
    }
    if ((!runtime.instances.empty() || !runtime.meshes.empty() ||
         runtime.state == CellState::Evicting) &&
        !rollbackCell(key)) {
        return false;
    }
    if (!canCommitCell(key, composites)) {
        runtime.state = CellState::Loading;
        return false;
    }

    LARGE_INTEGER readinessEnd = {};
    QueryPerformanceCounter(&readinessEnd);

    runtime.state = CellState::Loading;
    const auto& cell = catalog_.cells[catalogCell->second];
    std::unordered_set<std::uint64_t> acquired;
    auto acquire = [&](const RetainedCatalog::Mesh& mesh,
                       ResidentCompositeCache* cache,
                       CellId terrainCell) {
        if (!acquired.insert(mesh.identity).second) {
            return true;
        }
        std::uint64_t identity = 0;
        if (!acquireMesh(
                mesh, cache, terrainCell,
                settings_.transitionBudgetBytes(), identity)) {
            acquired.erase(mesh.identity);
            return false;
        }
        runtime.meshes.push_back(identity);
        return true;
    };

    const CellId terrainCell{ key.x, key.y };
    for (std::uint32_t i = 0; i < cell.terrainMeshCount; ++i) {
        if (!acquire(
                catalog_.meshes[cell.terrainMeshFirst + i],
                &composites,
                terrainCell)) {
            rollbackCell(key);
            return false;
        }
    }
    std::uint32_t enabledStaticPlacements = 0;
    for (std::uint32_t i = 0; i < cell.staticPlacementCount; ++i) {
        const auto& placement = catalog_.placements[cell.staticPlacementFirst + i];
        if ((placement.flags & 1u) == 0) {
            continue;
        }
        ++enabledStaticPlacements;
        const auto& mesh = catalog_.meshes[catalog_.meshByIdentity.at(placement.prototypeIdentity)];
        if (!acquire(mesh, nullptr, {})) {
            rollbackCell(key);
            return false;
        }
    }
    runtime.state = CellState::MeshReady;

    LARGE_INTEGER resourcesEnd = {};
    QueryPerformanceCounter(&resourcesEnd);

    auto createInstance = [&](std::uint64_t identity, const remixapi_InstanceInfo& info,
                              std::uint64_t meshIdentity) {
        auto* api = remix();
        if (!api || liveInstanceIdentities_.find(identity) != liveInstanceIdentities_.end()) {
            return false;
        }
        remixapi_InstanceHandle handle = nullptr;
        if (api->CreateRetainedInstance(identity, &info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS ||
            !handle) {
            return false;
        }
        liveInstanceIdentities_.insert(identity);
        runtime.instances.push_back({ handle, identity, meshIdentity });
        return true;
    };

    for (std::uint32_t i = 0; i < cell.terrainMeshCount; ++i) {
        const auto& mesh = catalog_.meshes[cell.terrainMeshFirst + i];
        remixapi_InstanceInfo info = {};
        info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        info.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN;
        info.mesh = meshResources_.at(mesh.identity).handle;
        info.transform.matrix[0][0] = 1.0f;
        info.transform.matrix[1][1] = 1.0f;
        info.transform.matrix[2][2] = 1.0f;
        info.transform.matrix[2][3] = -40.0f;
        const auto identity = domainIdentity(mesh.identity, 0x5445525241494e49ull);
        if (!createInstance(identity, info, mesh.identity)) {
            rollbackCell(key);
            return false;
        }
    }

    for (std::uint32_t i = 0; i < cell.staticPlacementCount; ++i) {
        const auto& placement = catalog_.placements[cell.staticPlacementFirst + i];
        if ((placement.flags & 1u) == 0) {
            continue;
        }
        const auto* matrix = placement.transform;
        remixapi_InstanceInfo info = {};
        info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        info.mesh = meshResources_.at(placement.prototypeIdentity).handle;
        info.transform.matrix[0][0] = matrix[0];
        info.transform.matrix[0][1] = matrix[4];
        info.transform.matrix[0][2] = matrix[8];
        info.transform.matrix[0][3] = matrix[12];
        info.transform.matrix[1][0] = matrix[1];
        info.transform.matrix[1][1] = matrix[5];
        info.transform.matrix[1][2] = matrix[9];
        info.transform.matrix[1][3] = matrix[13];
        info.transform.matrix[2][0] = matrix[2];
        info.transform.matrix[2][1] = matrix[6];
        info.transform.matrix[2][2] = matrix[10];
        info.transform.matrix[2][3] = matrix[14];
        info.doubleSided = 1;
        const auto identity = domainIdentity(placement.identity, 0x535441544943494eull);
        if (!createInstance(identity, info, placement.prototypeIdentity)) {
            rollbackCell(key);
            return false;
        }
    }

    LARGE_INTEGER instancesEnd = {};
    QueryPerformanceCounter(&instancesEnd);

    runtime.state = CellState::Committed;
    LOG::logline(
        "RetainedWorld: committed cell (%d,%d), terrain=%u statics=%u meshes=%u instances=%u, commit_us ready=%llu resources=%llu instances=%llu total=%llu",
        key.x, key.y,
        static_cast<unsigned>(cell.terrainMeshCount),
        static_cast<unsigned>(enabledStaticPlacements),
        static_cast<unsigned>(runtime.meshes.size()),
        static_cast<unsigned>(runtime.instances.size()),
        static_cast<unsigned long long>(elapsedMicroseconds(commitStart, readinessEnd)),
        static_cast<unsigned long long>(elapsedMicroseconds(readinessEnd, resourcesEnd)),
        static_cast<unsigned long long>(elapsedMicroseconds(resourcesEnd, instancesEnd)),
        static_cast<unsigned long long>(elapsedMicroseconds(commitStart, instancesEnd)));
    return true;
}

bool RetainedWorldManager::rollbackCell(CellKey key) {
    auto runtimeIt = cells_.find(key);
    if (runtimeIt == cells_.end()) {
        return true;
    }
    auto& runtime = runtimeIt->second;
    // Legacy suppression ends when teardown starts. A failed remote destroy can briefly
    // duplicate geometry, but keeping suppression after partial teardown would create holes.
    runtime.state = CellState::Evicting;
    auto* api = remix();

    while (!runtime.instances.empty()) {
        const auto& instance = runtime.instances.back();
        if (!api || !api->DestroyRetainedInstance || !instance.handle) {
            return false;
        }
        const auto result = api->DestroyRetainedInstance(instance.handle);
        if (result != REMIXAPI_ERROR_CODE_SUCCESS) {
            LOG::logline(
                "RetainedWorld: DestroyRetainedInstance failed for identity %llu in cell (%d,%d) (error %u)",
                static_cast<unsigned long long>(instance.identity),
                key.x, key.y,
                static_cast<unsigned>(result));
            return false;
        }
        liveInstanceIdentities_.erase(instance.identity);
        runtime.instances.pop_back();
    }

    while (!runtime.meshes.empty()) {
        if (!releaseMesh(runtime.meshes.back())) {
            return false;
        }
        runtime.meshes.pop_back();
    }
    if (!releaseUnusedMaterials()) {
        return false;
    }

    runtime.state = CellState::Absent;
    return true;
}

bool RetainedWorldManager::evictCell(CellKey key) {
    const auto runtime = cells_.find(key);
    if (runtime == cells_.end() || !isRetainedCellActive(runtime->second)) {
        return true;
    }
    if (!rollbackCell(key)) {
        return false;
    }
    LOG::logline("RetainedWorld: evicted cell (%d,%d)", key.x, key.y);
    return true;
}

void RetainedWorldManager::reconcile(
    const D3DXVECTOR4& eye,
    const D3DXMATRIX& view,
    ResidentCompositeCache& composites) {
    (void)view;
    if (!initialized_ || !deviceReady_ || !catalogActive_ || catalogRefreshRequested_) {
        return;
    }

    if (!releaseUnusedMaterials()) {
        LOG::logline(
            "RetainedWorld: deferred cell commits until unused materials can be released");
        return;
    }

    const auto& target = chooseTargetCells(eye);
    const std::unordered_set<CellKey, CellKeyHash> desired(target.begin(), target.end());
    if (countObsoleteCells(cells_, desired) != 0) {
        return;
    }

    std::uint32_t committedThisFrame = 0;
    for (const auto key : target) {
        if (committedThisFrame >= settings_.commitsPerFrame) {
            break;
        }

        const auto runtime = cells_.find(key);
        if (runtime == cells_.end() || runtime->second.state == CellState::Committed) {
            continue;
        }
        if (!canCommitCell(key, composites)) {
            continue;
        }
        if (!commitCell(key, composites)) {
            LOG::logline(
                "RetainedWorld: deferred remaining cell commits after cell (%d,%d) failed",
                key.x, key.y);
            break;
        }
        ++committedThisFrame;
    }

    if (!releaseUnusedMaterials()) {
        LOG::logline("RetainedWorld: unused material cleanup remains pending");
    }
    composites.trimToBudget();

    if (resourceBytes_ > settings_.transitionBudgetBytes()) {
        LOG::logline(
            "RetainedWorld: transition resource budget invariant violated (%llu/%llu bytes)",
            static_cast<unsigned long long>(resourceBytes_),
            static_cast<unsigned long long>(settings_.transitionBudgetBytes()));
    }
}

bool RetainedWorldManager::isCellCommitted(
    std::int32_t cellX,
    std::int32_t cellY) const {
    const auto cell = cells_.find(CellKey{ cellX, cellY });
    return cell != cells_.end() && cell->second.state == CellState::Committed;
}

bool RetainedWorldManager::teardownCells() {
    bool success = true;
    for (auto& entry : cells_) {
        auto& runtime = entry.second;
        if (runtime.state != CellState::Absent ||
            !runtime.instances.empty() || !runtime.meshes.empty()) {
            success = rollbackCell(entry.first) && success;
        }
    }
    success = releaseUnusedMaterials() && success;

    const bool balanced = liveInstanceIdentities_.empty() &&
        meshResources_.empty() && materialResources_.empty() &&
        resourceBytes_ == 0;
    if (!balanced) {
        LOG::logline(
            "RetainedWorld: teardown incomplete (%u instances, %u meshes, %u materials, %llu bytes)",
            static_cast<unsigned>(liveInstanceIdentities_.size()),
            static_cast<unsigned>(meshResources_.size()),
            static_cast<unsigned>(materialResources_.size()),
            static_cast<unsigned long long>(resourceBytes_));
    }
    return success && balanced;
}

bool RetainedWorldManager::beforeDeviceReset() {
    if (!teardownCells()) {
        LOG::logline("RetainedWorld: device reset blocked by retained resource destruction failure");
        return false;
    }
    deviceReady_ = false;
    device_ = nullptr;
    return true;
}

void RetainedWorldManager::afterDeviceReset(
    IDirect3DDevice9* device,
    bool resetSucceeded) {
    device_ = resetSucceeded ? device : nullptr;
    deviceReady_ = resetSucceeded && device != nullptr;
    anchorValid_ = false;
    targetValid_ = false;
    admissionDeferred_ = false;
    targetRadius_ = 0;
    targetInnerRadius_ = 0;
    targetCells_.clear();
    targetResourceBytes_ = 0;
    targetCompositeBytes_ = 0;
}

bool RetainedWorldManager::shutdown() {
    if (!teardownCells()) {
        LOG::logline("RetainedWorld: shutdown deferred by retained resource destruction failure");
        return false;
    }

    clearCatalog();
    deviceReady_ = false;
    device_ = nullptr;
    catalogAttempted_ = false;
    catalogRefreshRequested_ = false;
    catalogRetryCounter_ = 0;
    refreshCounter_ = 0;
    worldspaceKey_.clear();
    const bool catalogVectorsReleased = releaseCatalogVectors();
    const bool deferredVectorsReleased = !client_ || client_->releaseDeferredVecs();
    if (!catalogVectorsReleased || !deferredVectorsReleased) {
        LOG::logline("RetainedWorld: shutdown deferred by catalog vector release failure");
        return false;
    }

    initialized_ = false;
    nextSettingsPollMs_ = 0;
    client_ = nullptr;
    return true;
}

} // namespace

namespace RetainedWorld {

bool initialize(IPC::Client& client, IDirect3DDevice9* device) {
    return g_manager.initialize(client, device);
}

void selectWorldspace(
    IPC::Client& client,
    const std::string& key,
    bool available,
    bool exterior) {
    g_manager.selectWorldspace(client, key, available, exterior);
}

void setupCamera(
    const D3DXVECTOR4& eye,
    const D3DXMATRIX& view,
    const D3DXMATRIX& projection,
    float nearPlane,
    float farPlane,
    bool exterior) {
    g_manager.setupCamera(eye, view, projection, nearPlane, farPlane, exterior);
}

void prepareCompositeTransition(
    const D3DXVECTOR4& eye,
    const D3DXMATRIX& view,
    ResidentCompositeCache& composites) {
    g_manager.prepareCompositeTransition(eye, view, composites);
}

void reconcile(
    const D3DXVECTOR4& eye,
    const D3DXMATRIX& view,
    ResidentCompositeCache& composites) {
    g_manager.reconcile(eye, view, composites);
}

bool isCellCommitted(std::int32_t cellX, std::int32_t cellY) {
    return g_manager.isCellCommitted(cellX, cellY);
}

void requestCatalogRefresh() {
    g_manager.requestCatalogRefresh();
}

bool beforeDeviceReset() {
    return g_manager.beforeDeviceReset();
}

void afterDeviceReset(IDirect3DDevice9* device, bool resetSucceeded) {
    g_manager.afterDeviceReset(device, resetSucceeded);
}

bool shutdown() {
    return g_manager.shutdown();
}

} // namespace RetainedWorld

#endif
