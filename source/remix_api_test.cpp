/**
 * remix_api_test.cpp — Feasibility test for Remix SDK API from 32-bit bridge client
 *
 * Tests whether remixapi_lib_loadRemixDllAndInitialize succeeds from our
 * 32-bit d3d8to9 wrapper when running through the Remix Bridge.
 * Requires exposeRemixApi = True in bridge.conf.
 */

#ifdef MGE_RTX

// Suppress d3d8to9's LOG before including anything
#ifndef D3D8TO9NOLOG
#define D3D8TO9NOLOG
#endif

// Allow 32-bit usage — the Remix Bridge forwards API calls to the 64-bit server
#define REMIX_ALLOW_X86

#include "remix_api_test.h"
#include "support/log.h"
#include <d3d9.h>

// Include the Remix SDK C header
#include "remix_c.h"

namespace RemixAPITest {

static remixapi_Interface g_remix = {};
static remixapi_HMODULE g_remixDll = nullptr;
static bool g_initialized = false;

bool initialize() {
    if (g_initialized) return true;

    LOG::logline("RemixAPI: Attempting to initialize from 32-bit bridge client...");

    remixapi_ErrorCode status = remixapi_lib_loadRemixDllAndInitialize(
        L"d3d9.dll",
        &g_remix,
        &g_remixDll
    );

    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        LOG::logline("RemixAPI: FAILED to initialize. Error code: %d", (int)status);
        LOG::logline("RemixAPI: Make sure exposeRemixApi = True in bridge.conf");
        return false;
    }

    LOG::logline("RemixAPI: SUCCESS! Interface initialized from 32-bit client.");

    // Log which functions are available
    LOG::logline("RemixAPI:   CreateMesh = %p", g_remix.CreateMesh);
    LOG::logline("RemixAPI:   DestroyMesh = %p", g_remix.DestroyMesh);
    LOG::logline("RemixAPI:   CreateMaterial = %p", g_remix.CreateMaterial);
    LOG::logline("RemixAPI:   DestroyMaterial = %p", g_remix.DestroyMaterial);
    LOG::logline("RemixAPI:   DrawInstance = %p", g_remix.DrawInstance);
    LOG::logline("RemixAPI:   SetupCamera = %p", g_remix.SetupCamera);
    LOG::logline("RemixAPI:   CreateLight = %p", g_remix.CreateLight);
    LOG::logline("RemixAPI:   DrawLightInstance = %p", g_remix.DrawLightInstance);
    LOG::logline("RemixAPI:   SetConfigVariable = %p", g_remix.SetConfigVariable);
    // Batched mesh + texture-hash entry points used by the live static batcher.
    LOG::logline("RemixAPI:   CreateMeshBatched = %p", g_remix.CreateMeshBatched);
    LOG::logline("RemixAPI:   dxvk_GetTextureHash = %p", g_remix.dxvk_GetTextureHash);
    LOG::logline("RemixAPI:   CreateRetainedInstance = %p", g_remix.CreateRetainedInstance);
    LOG::logline("RemixAPI:   UpdateRetainedInstance = %p", g_remix.UpdateRetainedInstance);
    LOG::logline("RemixAPI:   DestroyRetainedInstance = %p", g_remix.DestroyRetainedInstance);

    g_initialized = true;

    return true;
}

bool isInitialized() {
    return g_initialized;
}

bool supportsRetainedInstances() {
    return g_initialized &&
        g_remix.CreateMaterial &&
        g_remix.DestroyMaterial &&
        g_remix.CreateMesh &&
        g_remix.DestroyMesh &&
        g_remix.SetupCamera &&
        g_remix.dxvk_GetTextureHash &&
        g_remix.CreateRetainedInstance &&
        g_remix.UpdateRetainedInstance &&
        g_remix.DestroyRetainedInstance;
}

remixapi_Interface* getInterface() {
    return g_initialized ? &g_remix : nullptr;
}

void shutdown() {
    if (g_initialized) {
        LOG::logline("RemixAPI: Shutting down...");
        remixapi_lib_shutdownAndUnloadRemixDll(&g_remix, g_remixDll);
        g_remixDll = nullptr;
        g_initialized = false;
    }
}

} // namespace RemixAPITest

#endif // MGE_RTX
