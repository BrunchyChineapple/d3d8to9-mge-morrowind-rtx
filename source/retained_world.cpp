#ifdef MGE_RTX

#ifndef REMIX_ALLOW_X86
#define REMIX_ALLOW_X86
#endif

#include "retained_world.h"

#include "ipc/client.h"
#include "ipc/retainedcatalog.h"
#include "mge/compositecache.h"
#include "remix_api_test.h"
#include "remix_c.h"
#include "support/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
constexpr std::uint64_t kResourceBudgetBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t kTransitionResourceBudgetBytes = 1024ull * 1024ull * 1024ull;
constexpr std::uint32_t kCatalogRetryFrames = 60;
constexpr std::uint32_t kCatalogRefreshFrames = 300;
constexpr float kCellSize = 8192.0f;
constexpr float kCellHysteresis = 512.0f;
constexpr float kDirectionHysteresis = 0.25f;
constexpr std::uint32_t kAllowedMeshFlags =
    RetainedCatalog::MeshFlagHasAlpha |
    RetainedCatalog::MeshFlagAnimatedUv |
    RetainedCatalog::MeshFlagIndex32 |
    RetainedCatalog::MeshFlagCompositeDxt1;

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

std::wstring widenPath(const std::uint8_t* bytes, std::uint32_t byteCount) {
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
    if (encodedBytes == 0 ||
        encodedBytes > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return {};
    }

    const int inputBytes = static_cast<int>(encodedBytes);
    const int chars = MultiByteToWideChar(
        CP_ACP, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes),
        inputBytes, nullptr, 0);
    if (chars <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(chars), L'\0');
    if (MultiByteToWideChar(
            CP_ACP, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(bytes),
            inputBytes, result.data(), chars) != chars) {
        return {};
    }
    return result;
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
        float farPlane);
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
    bool requestCatalog(CatalogSnapshot& result);
    bool validateCatalog(CatalogSnapshot& candidate) const;
    bool canCommitCell(CellKey key, ResidentCompositeCache& composites) const;
    bool calculateTargetResourceBytes(
        const std::vector<CellKey>& target,
        std::uint64_t& finalBytes,
        std::uint64_t& stagingBytes) const;
    bool targetFitsFinalBudget(const std::vector<CellKey>& target) const;
    bool targetFitsCompositeBudget(
        const std::vector<CellKey>& target,
        const ResidentCompositeCache& composites) const;
    bool canStageTarget(
        const std::vector<CellKey>& target,
        ResidentCompositeCache& composites) const;
    bool commitCell(CellKey key, ResidentCompositeCache& composites);
    bool rollbackCell(CellKey key);
    bool evictCell(CellKey key);
    bool teardownCells();
    void clearCatalog();
    std::vector<CellKey> chooseTargetCells(
        const D3DXVECTOR4& eye,
        const D3DXMATRIX& view);
    CellKey chooseAnchor(const D3DXVECTOR4& eye);
    bool chooseNeighbor(CellKey anchor, const D3DXMATRIX& view, CellKey& neighbor);
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
    bool neighborValid_ = false;
    std::uint32_t catalogRetryCounter_ = 0;
    std::uint32_t refreshCounter_ = 0;
    IPC::Client* client_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    std::string worldspaceKey_;
    CellKey anchor_ = {};
    CellKey neighbor_ = {};

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

bool RetainedWorldManager::requestCatalog(CatalogSnapshot& result) {
    result.clear();
    if (!client_ || !client_->getRetainedWorldCatalogBlocking(
            headerId_, cellsId_, meshesId_, placementsId_, blobId_) ||
        headerView_.size() != 1) {
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
            !checkedRange(mesh.materialOffset, mesh.materialBytes, candidate.blob.size()) ||
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
                (mesh.materialBytes != 0 &&
                 ((mesh.flags & RetainedCatalog::MeshFlagCompositeDxt1) == 0 ||
                  mesh.materialIdentity == 0))) {
                return false;
            }
        } else if (mesh.category == RetainedCatalog::Category::Static) {
            if (mesh.indexStride != 2 ||
                (mesh.flags & (RetainedCatalog::MeshFlagIndex32 |
                               RetainedCatalog::MeshFlagCompositeDxt1)) != 0 ||
                mesh.materialIdentity == 0 || mesh.materialBytes == 0) {
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
    catalogAttempted_ = true;
    refreshCounter_ = 0;
    if (!requestCatalog(candidate)) {
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
    neighborValid_ = false;
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
    neighborValid_ = false;
    catalog_.clear();
    cells_.clear();
}

void RetainedWorldManager::setupCamera(
    const D3DXVECTOR4& eye,
    const D3DXMATRIX& view,
    const D3DXMATRIX& projection,
    float nearPlane,
    float farPlane) {
    auto* api = remix();
    if (!api || !api->SetupCamera || nearPlane <= 0.0f || farPlane <= nearPlane ||
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
    parameters.farPlane = farPlane;

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
        return anchor_;
    }

    const std::int64_t dx = static_cast<std::int64_t>(raw.x) - anchor_.x;
    const std::int64_t dy = static_cast<std::int64_t>(raw.y) - anchor_.y;
    if (std::abs(dx) + std::abs(dy) != 1) {
        anchor_ = raw;
        neighborValid_ = false;
        return anchor_;
    }

    bool crossed = false;
    if (dx > 0) crossed = eye.x >= (anchor_.x + 1) * kCellSize + kCellHysteresis;
    if (dx < 0) crossed = eye.x < anchor_.x * kCellSize - kCellHysteresis;
    if (dy > 0) crossed = eye.y >= (anchor_.y + 1) * kCellSize + kCellHysteresis;
    if (dy < 0) crossed = eye.y < anchor_.y * kCellSize - kCellHysteresis;
    if (crossed) {
        anchor_ = raw;
        neighborValid_ = false;
    }
    return anchor_;
}

bool RetainedWorldManager::chooseNeighbor(
    CellKey anchor,
    const D3DXMATRIX& view,
    CellKey& neighbor) {
    const std::array<CellKey, 4> candidates = {{
        { anchor.x + 1, anchor.y },
        { anchor.x - 1, anchor.y },
        { anchor.x, anchor.y + 1 },
        { anchor.x, anchor.y - 1 },
    }};
    const std::array<std::pair<float, float>, 4> directions = {{
        { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f }
    }};

    float forwardX = view._13;
    float forwardY = view._23;
    const float length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
    if (length > 1.0e-5f) {
        forwardX /= length;
        forwardY /= length;
    } else {
        forwardX = 1.0f;
        forwardY = 0.0f;
    }

    int best = -1;
    float bestScore = -std::numeric_limits<float>::infinity();
    int previous = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        if (catalog_.cellByKey.find(candidates[i]) == catalog_.cellByKey.end()) {
            continue;
        }
        const float score = forwardX * directions[i].first + forwardY * directions[i].second;
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
        if (neighborValid_ && candidates[i] == neighbor_) {
            previous = i;
        }
    }
    if (best < 0) {
        return false;
    }

    if (previous >= 0) {
        const float previousScore =
            forwardX * directions[previous].first + forwardY * directions[previous].second;
        if (bestScore - previousScore < kDirectionHysteresis) {
            best = previous;
        }
    }

    neighbor = candidates[best];
    neighbor_ = neighbor;
    neighborValid_ = true;
    return true;
}

std::vector<CellKey> RetainedWorldManager::chooseTargetCells(
    const D3DXVECTOR4& eye,
    const D3DXMATRIX& view) {
    std::vector<CellKey> target;
    const CellKey anchor = chooseAnchor(eye);
    if (catalog_.cellByKey.find(anchor) == catalog_.cellByKey.end()) {
        return target;
    }
    target.push_back(anchor);
    CellKey neighbor;
    if (!chooseNeighbor(anchor, view, neighbor)) {
        target.clear();
        return target;
    }
    target.push_back(neighbor);
    return target;
}

void RetainedWorldManager::prepareCompositeTransition(
    const D3DXVECTOR4& eye,
    const D3DXMATRIX& view,
    ResidentCompositeCache& composites) {
    VisibleCellSet transitionTargets;
    if (initialized_ && deviceReady_ && catalogActive_ && !catalogRefreshRequested_) {
        const auto target = chooseTargetCells(eye, view);
        if (target.size() == 2 && targetFitsCompositeBudget(target, composites)) {
            std::unordered_set<CellKey, CellKeyHash> desired(target.begin(), target.end());
            std::size_t missingComposites = 0;
            for (const auto key : target) {
                const CellId cell{ key.x, key.y };
                transitionTargets.insert(cell);
                if (composites.lookup(cell) == nullptr) {
                    ++missingComposites;
                }
            }

            bool hasObsoleteCell = false;
            for (const auto& entry : cells_) {
                if ((entry.second.state != CellState::Absent ||
                     !entry.second.instances.empty() || !entry.second.meshes.empty()) &&
                    desired.find(entry.first) == desired.end()) {
                    hasObsoleteCell = true;
                    break;
                }
            }
            if (missingComposites > 1 && hasObsoleteCell && !teardownCells()) {
                LOG::logline("RetainedWorld: disjoint transition teardown incomplete; retaining legacy rendering");
                transitionTargets.clear();
            }
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
            float u = (vertex.position[0] - cellMinX) / kCellSize;
            float v = (vertex.position[1] - cellMinY) / kCellSize;
            u -= std::floor(u);
            v -= std::floor(v);
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

    IDirect3DTexture9* terrainTexture = nullptr;
    bool hasCompositeLease = false;
    std::wstring albedoPath;
    std::array<wchar_t, 24> textureHashPath = {};
    std::uint64_t materialHash = mesh.materialIdentity;
    if (mesh.category == RetainedCatalog::Category::Terrain) {
        if (!composites) {
            return false;
        }
        terrainTexture = composites->pin(terrainCell);
        if (!terrainTexture) {
            return false;
        }
        hasCompositeLease = true;

        std::uint64_t textureHash = 0;
        if (!api->dxvk_GetTextureHash ||
            api->dxvk_GetTextureHash(terrainTexture, &textureHash) != REMIXAPI_ERROR_CODE_SUCCESS ||
            textureHash == 0) {
            composites->unpin(terrainCell);
            return false;
        }
        swprintf_s(
            textureHashPath.data(), textureHashPath.size(),
            L"0x%016llX", static_cast<unsigned long long>(textureHash));
        materialHash = textureHash;
    } else {
        albedoPath = widenPath(
            catalog_.blob.data() + mesh.materialOffset,
            mesh.materialBytes);
        if (albedoPath.empty()) {
            return false;
        }
    }

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
    info.hash = materialHash;
    info.albedoTexture = mesh.category == RetainedCatalog::Category::Terrain
        ? textureHashPath.data()
        : albedoPath.c_str();
    info.filterMode = 1;
    info.wrapModeU = 1;
    info.wrapModeV = 1;

    if (api->CreateMaterial(&info, &output.handle) != REMIXAPI_ERROR_CODE_SUCCESS ||
        !output.handle) {
        if (hasCompositeLease) {
            composites->unpin(terrainCell);
        }
        output = {};
        return false;
    }
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

    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
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

bool RetainedWorldManager::calculateTargetResourceBytes(
    const std::vector<CellKey>& target,
    std::uint64_t& finalBytes,
    std::uint64_t& stagingBytes) const {
    finalBytes = 0;
    stagingBytes = resourceBytes_;
    std::unordered_set<std::uint64_t> requiredMeshes;
    for (const auto key : target) {
        const auto catalogCell = catalog_.cellByKey.find(key);
        if (catalogCell == catalog_.cellByKey.end()) {
            return false;
        }
        const auto& cell = catalog_.cells[catalogCell->second];
        for (std::uint32_t i = 0; i < cell.terrainMeshCount; ++i) {
            requiredMeshes.insert(catalog_.meshes[cell.terrainMeshFirst + i].identity);
        }
        for (std::uint32_t i = 0; i < cell.staticPlacementCount; ++i) {
            const auto& placement = catalog_.placements[cell.staticPlacementFirst + i];
            if ((placement.flags & 1u) != 0) {
                requiredMeshes.insert(placement.prototypeIdentity);
            }
        }
    }

    const auto checkedAdd = [](std::uint64_t& total, std::uint64_t value) {
        if (total > std::numeric_limits<std::uint64_t>::max() - value) {
            return false;
        }
        total += value;
        return true;
    };
    for (const auto identity : requiredMeshes) {
        const auto meshIndex = catalog_.meshByIdentity.find(identity);
        if (meshIndex == catalog_.meshByIdentity.end()) {
            return false;
        }
        const auto& mesh = catalog_.meshes[meshIndex->second];
        std::uint64_t bytes = 0;
        if (!checkedAdd(
                bytes,
                static_cast<std::uint64_t>(mesh.vertexCount) * sizeof(remixapi_HardcodedVertex)) ||
            !checkedAdd(
                bytes,
                static_cast<std::uint64_t>(mesh.indexCount) * sizeof(std::uint32_t)) ||
            !checkedAdd(finalBytes, bytes)) {
            return false;
        }
        if (meshResources_.find(identity) == meshResources_.end() &&
            !checkedAdd(stagingBytes, bytes)) {
            return false;
        }
    }
    return true;
}

bool RetainedWorldManager::targetFitsFinalBudget(
    const std::vector<CellKey>& target) const {
    std::uint64_t finalBytes = 0;
    std::uint64_t stagingBytes = 0;
    return calculateTargetResourceBytes(target, finalBytes, stagingBytes) &&
        finalBytes <= kResourceBudgetBytes;
}

bool RetainedWorldManager::targetFitsCompositeBudget(
    const std::vector<CellKey>& target,
    const ResidentCompositeCache& composites) const {
    std::uint64_t totalBytes = 0;
    for (const auto key : target) {
        const auto catalogCell = catalog_.cellByKey.find(key);
        if (catalogCell == catalog_.cellByKey.end()) {
            return false;
        }
        const auto& cell = catalog_.cells[catalogCell->second];
        std::uint32_t cellBytes = 0;
        for (std::uint32_t i = 0; i < cell.terrainMeshCount; ++i) {
            cellBytes = std::max(
                cellBytes,
                catalog_.meshes[cell.terrainMeshFirst + i].materialBytes);
        }
        if (totalBytes > std::numeric_limits<std::uint64_t>::max() - cellBytes) {
            return false;
        }
        totalBytes += cellBytes;
    }
    return totalBytes <= composites.budgetBytes();
}

bool RetainedWorldManager::canStageTarget(
    const std::vector<CellKey>& target,
    ResidentCompositeCache& composites) const {
    for (const auto key : target) {
        if (!canCommitCell(key, composites)) {
            return false;
        }
    }

    std::uint64_t finalBytes = 0;
    std::uint64_t stagingBytes = 0;
    return calculateTargetResourceBytes(target, finalBytes, stagingBytes) &&
        stagingBytes <= kTransitionResourceBudgetBytes;
}

bool RetainedWorldManager::commitCell(
    CellKey key,
    ResidentCompositeCache& composites) {
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
                kTransitionResourceBudgetBytes, identity)) {
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
    for (std::uint32_t i = 0; i < cell.staticPlacementCount; ++i) {
        const auto& placement = catalog_.placements[cell.staticPlacementFirst + i];
        if ((placement.flags & 1u) == 0) {
            continue;
        }
        const auto& mesh = catalog_.meshes[catalog_.meshByIdentity.at(placement.prototypeIdentity)];
        if (!acquire(mesh, nullptr, {})) {
            rollbackCell(key);
            return false;
        }
    }
    runtime.state = CellState::MeshReady;

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

    runtime.state = CellState::Committed;
    LOG::logline(
        "RetainedWorld: committed cell (%d,%d), %u meshes, %u instances",
        key.x, key.y,
        static_cast<unsigned>(runtime.meshes.size()),
        static_cast<unsigned>(runtime.instances.size()));
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
    if (runtime == cells_.end() || runtime->second.state == CellState::Absent) {
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
    if (!initialized_ || !deviceReady_ || !catalogActive_ || catalogRefreshRequested_) {
        return;
    }

    releaseUnusedMaterials();
    const auto target = chooseTargetCells(eye, view);
    if (target.size() != 2) {
        teardownCells();
        composites.setTransitionTargets({});
        composites.trimToBudget();
        return;
    }
    if (!targetFitsFinalBudget(target)) {
        LOG::logline("RetainedWorld: target pair exceeds the final mesh resource budget; legacy rendering active");
        teardownCells();
        composites.setTransitionTargets({});
        composites.trimToBudget();
        return;
    }
    if (!targetFitsCompositeBudget(target, composites)) {
        LOG::logline("RetainedWorld: target pair exceeds the final composite budget; legacy rendering active");
        teardownCells();
        composites.setTransitionTargets({});
        composites.trimToBudget();
        return;
    }
    if (!canStageTarget(target, composites)) {
        return;
    }

    std::vector<CellKey> newlyCommitted;
    for (const auto key : target) {
        auto& runtime = cells_.at(key);
        if (runtime.state == CellState::Committed) {
            continue;
        }
        if (!commitCell(key, composites)) {
            for (auto rollback = newlyCommitted.rbegin(); rollback != newlyCommitted.rend(); ++rollback) {
                rollbackCell(*rollback);
            }
            return;
        }
        newlyCommitted.push_back(key);
    }

    std::unordered_set<CellKey, CellKeyHash> desired(target.begin(), target.end());
    std::vector<CellKey> evictions;
    for (const auto& entry : cells_) {
        if (entry.second.state != CellState::Absent &&
            desired.find(entry.first) == desired.end()) {
            evictions.push_back(entry.first);
        }
    }

    bool evictionSucceeded = true;
    for (const auto key : evictions) {
        evictionSucceeded = evictCell(key) && evictionSucceeded;
    }
    evictionSucceeded = releaseUnusedMaterials() && evictionSucceeded;
    composites.setTransitionTargets({});
    composites.trimToBudget();
    if (evictionSucceeded && resourceBytes_ > kResourceBudgetBytes) {
        LOG::logline(
            "RetainedWorld: final resource budget invariant violated (%llu bytes)",
            static_cast<unsigned long long>(resourceBytes_));
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
    neighborValid_ = false;
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
    float farPlane) {
    g_manager.setupCamera(eye, view, projection, nearPlane, farPlane);
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
