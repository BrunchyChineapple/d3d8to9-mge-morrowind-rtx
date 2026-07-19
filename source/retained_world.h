#pragma once

#ifdef MGE_RTX

#include <cstdint>
#include <string>

struct IDirect3DDevice9;
struct D3DXMATRIX;
struct D3DXVECTOR4;
class ResidentCompositeCache;

namespace IPC {
    class Client;
}

namespace RetainedWorld {
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
    bool isCellCommitted(std::int32_t cellX, std::int32_t cellY);
    void requestCatalogRefresh();
    bool beforeDeviceReset();
    void afterDeviceReset(IDirect3DDevice9* device, bool resetSucceeded);
    bool shutdown();
}

#endif
